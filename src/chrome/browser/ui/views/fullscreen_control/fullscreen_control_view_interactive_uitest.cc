// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/callback.h"
#include "base/macros.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_bubble_type.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_manager.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/fullscreen_control/fullscreen_control_host.h"
#include "chrome/browser/ui/views/fullscreen_control/fullscreen_control_view.h"
#include "chrome/common/chrome_features.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "chrome/test/views/scoped_macviews_browser_mode.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/web/web_fullscreen_options.h"
#include "ui/base/ui_base_features.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view.h"
#include "url/gurl.h"

#if defined(USE_AURA)
#include "ui/aura/test/test_cursor_client.h"
#include "ui/aura/window.h"
#endif

namespace {

constexpr base::TimeDelta kPopupEventTimeout = base::TimeDelta::FromSeconds(5);

// Observer for NOTIFICATION_FULLSCREEN_CHANGED notifications.
class FullscreenNotificationObserver
    : public content::WindowedNotificationObserver {
 public:
  FullscreenNotificationObserver()
      : WindowedNotificationObserver(
            chrome::NOTIFICATION_FULLSCREEN_CHANGED,
            content::NotificationService::AllSources()) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(FullscreenNotificationObserver);
};

}  // namespace

class FullscreenControlViewTest : public InProcessBrowserTest {
 public:
  FullscreenControlViewTest() = default;

  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {features::kFullscreenExitUI, features::kKeyboardLockAPI}, {});
    InProcessBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
#if defined(USE_AURA)
    // This code prevents WindowEventDispatcher from synthesizing mouse move
    // events when views get refreshed, so that they won't interfere with the
    // tests. Note that new mouse move events directly coming from the real
    // device will still pass through.
    auto* root_window = browser()->window()->GetNativeWindow()->GetRootWindow();
    cursor_client_ =
        std::make_unique<aura::test::TestCursorClient>(root_window);
    cursor_client_->DisableMouseEvents();
#endif
  }

  void TearDownOnMainThread() override {
#if defined(USE_AURA)
    cursor_client_.reset();
#endif
  }

 protected:
  FullscreenControlHost* GetFullscreenControlHost() {
    BrowserView* browser_view =
        BrowserView::GetBrowserViewForBrowser(browser());
    return browser_view->GetFullscreenControlHost();
  }

  FullscreenControlView* GetFullscreenControlView() {
    return GetFullscreenControlHost()->GetPopup()->control_view();
  }

  views::Button* GetFullscreenExitButton() {
    return GetFullscreenControlView()->exit_fullscreen_button();
  }

  ExclusiveAccessManager* GetExclusiveAccessManager() {
    return browser()->exclusive_access_manager();
  }

  KeyboardLockController* GetKeyboardLockController() {
    return GetExclusiveAccessManager()->keyboard_lock_controller();
  }

  content::WebContents* GetActiveWebContents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  bool IsPopupCreated() { return GetFullscreenControlHost()->IsPopupCreated(); }

  void EnterActiveTabFullscreen() {
    FullscreenNotificationObserver fullscreen_observer;
    content::WebContentsDelegate* delegate =
        static_cast<content::WebContentsDelegate*>(browser());
    delegate->EnterFullscreenModeForTab(GetActiveWebContents(),
                                        GURL("about:blank"),
                                        blink::WebFullscreenOptions());
    fullscreen_observer.Wait();
    ASSERT_TRUE(delegate->IsFullscreenForTabOrPending(GetActiveWebContents()));
  }

  void EnableKeyboardLock() {
    KeyboardLockController* controller = GetKeyboardLockController();
    controller->fake_keyboard_lock_for_test_ = true;
    controller->RequestKeyboardLock(GetActiveWebContents(),
                                    /*require_esc_to_exit=*/true);
    controller->fake_keyboard_lock_for_test_ = false;
  }

  void SetPopupVisibilityChangedCallback(base::OnceClosure callback) {
    GetFullscreenControlHost()->on_popup_visibility_changed_ =
        std::move(callback);
  }

  base::OneShotTimer* GetPopupTimeoutTimer() {
    return &GetFullscreenControlHost()->popup_timeout_timer_;
  }

  void RunLoopUntilVisibilityChanges() {
    base::RunLoop run_loop;
    SetPopupVisibilityChangedCallback(run_loop.QuitClosure());
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), kPopupEventTimeout);
    run_loop.Run();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
  test::ScopedMacViewsBrowserMode views_mode_{true};

#if defined(USE_AURA)
  std::unique_ptr<aura::test::TestCursorClient> cursor_client_;
#endif

  DISALLOW_COPY_AND_ASSIGN(FullscreenControlViewTest);
};

// Creating the popup on Mac increases the memory use by ~2MB so it should be
// lazily loaded only when necessary. This test verifies that the popup is not
// immediately created when FullscreenControlHost is created.
IN_PROC_BROWSER_TEST_F(FullscreenControlViewTest,
                       NoFullscreenPopupOnBrowserFullscreen) {
  EnterActiveTabFullscreen();
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  DCHECK(browser_view);
  ASSERT_TRUE(browser_view->IsFullscreen());
  ASSERT_FALSE(IsPopupCreated());
}

#if defined(OS_MACOSX)
// Entering fullscreen is flaky on Mac: http://crbug.com/824517
#define MAYBE_MouseExitFullscreen DISABLED_MouseExitFullscreen
#else
#define MAYBE_MouseExitFullscreen MouseExitFullscreen
#endif
IN_PROC_BROWSER_TEST_F(FullscreenControlViewTest, MAYBE_MouseExitFullscreen) {
  EnterActiveTabFullscreen();
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view->IsFullscreen());

  FullscreenControlHost* host = GetFullscreenControlHost();
  host->Hide(false);
  ASSERT_FALSE(host->IsVisible());

  // Simulate moving the mouse to the top of the screen, which should show the
  // fullscreen exit UI.
  ui::MouseEvent mouse_move(ui::ET_MOUSE_MOVED, gfx::Point(1, 1), gfx::Point(),
                            base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  ASSERT_TRUE(host->IsVisible());

  // Simulate clicking on the fullscreen exit button, which should cause the
  // browser to exit fullscreen and hide the fullscreen exit control.
  ui::MouseEvent mouse_click(ui::ET_MOUSE_PRESSED, gfx::Point(), gfx::Point(),
                             base::TimeTicks(), ui::EF_LEFT_MOUSE_BUTTON,
                             ui::EF_LEFT_MOUSE_BUTTON);
  GetFullscreenControlView()->ButtonPressed(GetFullscreenExitButton(),
                                            mouse_click);

  ASSERT_FALSE(host->IsVisible());
  ASSERT_FALSE(browser_view->IsFullscreen());
}

#if defined(OS_MACOSX)
// Entering fullscreen is flaky on Mac: http://crbug.com/824517
#define MAYBE_MouseExitFullscreen_TimeoutAndRetrigger \
  DISABLED_MouseExitFullscreen_TimeoutAndRetrigger
#else
#define MAYBE_MouseExitFullscreen_TimeoutAndRetrigger \
  MouseExitFullscreen_TimeoutAndRetrigger
#endif
IN_PROC_BROWSER_TEST_F(FullscreenControlViewTest,
                       MAYBE_MouseExitFullscreen_TimeoutAndRetrigger) {
  EnterActiveTabFullscreen();
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view->IsFullscreen());

  FullscreenControlHost* host = GetFullscreenControlHost();
  host->Hide(false);
  ASSERT_FALSE(host->IsVisible());

  // Simulate moving the mouse to the top of the screen, which should show the
  // fullscreen exit UI.
  ui::MouseEvent mouse_move(ui::ET_MOUSE_MOVED, gfx::Point(1, 1), gfx::Point(),
                            base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  ASSERT_TRUE(host->IsVisible());

  // Wait until popup times out. This is one wait for show and one wait for
  // hide.
  RunLoopUntilVisibilityChanges();
  RunLoopUntilVisibilityChanges();
  ASSERT_FALSE(host->IsVisible());

  // Simulate moving the mouse to the top again. This should not show the exit
  // UI.
  mouse_move = ui::MouseEvent(ui::ET_MOUSE_MOVED, gfx::Point(2, 1),
                              gfx::Point(), base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  ASSERT_FALSE(host->IsVisible());

  // Simulate moving the mouse out of the buffer area. This resets the state.
  mouse_move = ui::MouseEvent(ui::ET_MOUSE_MOVED, gfx::Point(2, 1000),
                              gfx::Point(), base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  ASSERT_FALSE(host->IsVisible());

  // Simulate moving the mouse to the top again, which should show the exit UI.
  mouse_move = ui::MouseEvent(ui::ET_MOUSE_MOVED, gfx::Point(1, 1),
                              gfx::Point(), base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  RunLoopUntilVisibilityChanges();
  ASSERT_TRUE(host->IsVisible());

  // Simulate immediately moving the mouse out of the buffer area. This should
  // hide the exit UI.
  mouse_move = ui::MouseEvent(ui::ET_MOUSE_MOVED, gfx::Point(2, 1000),
                              gfx::Point(), base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  RunLoopUntilVisibilityChanges();
  ASSERT_FALSE(host->IsVisible());

  ASSERT_TRUE(browser_view->IsFullscreen());
}

#if defined(OS_MACOSX)
// Entering fullscreen is flaky on Mac: http://crbug.com/824517
#define MAYBE_TouchPopupInteraction DISABLED_TouchPopupInteraction
#else
#define MAYBE_TouchPopupInteraction TouchPopupInteraction
#endif
IN_PROC_BROWSER_TEST_F(FullscreenControlViewTest, MAYBE_TouchPopupInteraction) {
  EnterActiveTabFullscreen();
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view->IsFullscreen());

  FullscreenControlHost* host = GetFullscreenControlHost();
  host->Hide(false);
  ASSERT_FALSE(host->IsVisible());

  // Simulate a short tap that doesn't trigger the popup.
  ui::TouchEvent touch_event(
      ui::ET_TOUCH_PRESSED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);

  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_RELEASED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);

  ASSERT_FALSE(host->IsVisible());

  // Simulate a press-and-hold.
  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_PRESSED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);

  ui::GestureEvent gesture(1, 1, 0, ui::EventTimeForNow(),
                           ui::GestureEventDetails(ui::ET_GESTURE_LONG_PRESS));
  host->OnGestureEvent(&gesture);

  // Wait until the popup is fully shown then release the touch.
  RunLoopUntilVisibilityChanges();
  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_RELEASED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);

  ASSERT_TRUE(host->IsVisible());
  // Wait for the popup to time out.
  RunLoopUntilVisibilityChanges();
  ASSERT_FALSE(host->IsVisible());

  // Simulate a press-and-hold again.
  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_PRESSED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);

  gesture =
      ui::GestureEvent(1, 1, 0, ui::EventTimeForNow(),
                       ui::GestureEventDetails(ui::ET_GESTURE_LONG_PRESS));
  host->OnGestureEvent(&gesture);

  RunLoopUntilVisibilityChanges();

  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_RELEASED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);
  ASSERT_TRUE(host->IsVisible());

  // Simulate pressing the button.
  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_PRESSED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  GetFullscreenControlView()->ButtonPressed(GetFullscreenExitButton(),
                                            touch_event);

  ASSERT_FALSE(host->IsVisible());
  ASSERT_FALSE(browser_view->IsFullscreen());
}

#if defined(OS_MACOSX)
// Entering fullscreen is flaky on Mac: http://crbug.com/824517
#define MAYBE_MouseAndTouchInteraction_NoInterference \
  DISABLED_MouseAndTouchInteraction_NoInterference
#else
#define MAYBE_MouseAndTouchInteraction_NoInterference \
  MouseAndTouchInteraction_NoInterference
#endif
IN_PROC_BROWSER_TEST_F(FullscreenControlViewTest,
                       MAYBE_MouseAndTouchInteraction_NoInterference) {
  EnterActiveTabFullscreen();
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view->IsFullscreen());

  FullscreenControlHost* host = GetFullscreenControlHost();
  host->Hide(false);
  ASSERT_FALSE(host->IsVisible());

  // Move cursor to the top.
  ui::MouseEvent mouse_move(ui::ET_MOUSE_MOVED, gfx::Point(1, 1), gfx::Point(),
                            base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  RunLoopUntilVisibilityChanges();
  ASSERT_TRUE(host->IsVisible());

  // Simulate a press-and-hold.
  ui::TouchEvent touch_event(
      ui::ET_TOUCH_PRESSED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);
  ui::GestureEvent gesture(1, 1, 0, ui::EventTimeForNow(),
                           ui::GestureEventDetails(ui::ET_GESTURE_LONG_PRESS));
  host->OnGestureEvent(&gesture);

  // Move cursor out of the buffer area, which should hide the exit UI.
  mouse_move = ui::MouseEvent(ui::ET_MOUSE_MOVED, gfx::Point(2, 1000),
                              gfx::Point(), base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  RunLoopUntilVisibilityChanges();
  ASSERT_FALSE(host->IsVisible());

  // Release the touch, which should have no effect.
  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_RELEASED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);
  ASSERT_FALSE(host->IsVisible());

  // Simulate a press-and-hold to trigger the UI.
  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_PRESSED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);
  gesture =
      ui::GestureEvent(1, 1, 0, ui::EventTimeForNow(),
                       ui::GestureEventDetails(ui::ET_GESTURE_LONG_PRESS));
  host->OnGestureEvent(&gesture);
  RunLoopUntilVisibilityChanges();
  ASSERT_TRUE(host->IsVisible());

  // Move the cursor to the top.
  mouse_move = ui::MouseEvent(ui::ET_MOUSE_MOVED, gfx::Point(1, 1),
                              gfx::Point(), base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  ASSERT_TRUE(host->IsVisible());

  // Move the cursor out of the buffer area, which will have no effect.
  mouse_move = ui::MouseEvent(ui::ET_MOUSE_MOVED, gfx::Point(2, 1000),
                              gfx::Point(), base::TimeTicks(), 0, 0);
  host->OnMouseEvent(&mouse_move);
  // This simply times out.
  RunLoopUntilVisibilityChanges();
  ASSERT_TRUE(host->IsVisible());

  // Release the touch. The UI should then timeout.
  touch_event = ui::TouchEvent(
      ui::ET_TOUCH_RELEASED, gfx::Point(1, 1), ui::EventTimeForNow(),
      ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH, 0));
  host->OnTouchEvent(&touch_event);
  RunLoopUntilVisibilityChanges();
  ASSERT_FALSE(host->IsVisible());
}

#if defined(OS_MACOSX)
// Entering fullscreen is flaky on Mac: http://crbug.com/824517
#define MAYBE_KeyboardPopupInteraction DISABLED_KeyboardPopupInteraction
#else
#define MAYBE_KeyboardPopupInteraction KeyboardPopupInteraction
#endif
IN_PROC_BROWSER_TEST_F(FullscreenControlViewTest,
                       MAYBE_KeyboardPopupInteraction) {
  EnterActiveTabFullscreen();
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view->IsFullscreen());

  FullscreenControlHost* host = GetFullscreenControlHost();
  host->Hide(false);
  ASSERT_FALSE(host->IsVisible());

  // Lock the keyboard and ensure it is active.
  EnableKeyboardLock();
  ASSERT_TRUE(GetKeyboardLockController()->IsKeyboardLockActive());

  // Verify a bubble message is now displayed, then dismiss it.
  ASSERT_NE(ExclusiveAccessBubbleType::EXCLUSIVE_ACCESS_BUBBLE_TYPE_NONE,
            GetExclusiveAccessManager()->GetExclusiveAccessExitBubbleType());
  host->Hide(/*animate=*/false);
  ASSERT_FALSE(host->IsVisible());

  base::RunLoop show_run_loop;
  SetPopupVisibilityChangedCallback(show_run_loop.QuitClosure());
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, show_run_loop.QuitClosure(), kPopupEventTimeout);

  // Send a key press event to show the popup.
  ui::KeyEvent key_down(ui::ET_KEY_PRESSED, ui::VKEY_ESCAPE, ui::EF_NONE);
  host->OnKeyEvent(&key_down);
  // Popup is not shown immediately.
  ASSERT_FALSE(host->IsVisible());

  show_run_loop.Run();
  ASSERT_TRUE(host->IsVisible());

  base::RunLoop hide_run_loop;
  SetPopupVisibilityChangedCallback(hide_run_loop.QuitClosure());
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, hide_run_loop.QuitClosure(), kPopupEventTimeout);

  // Send a key press event to hide the popup.
  ui::KeyEvent key_up(ui::ET_KEY_RELEASED, ui::VKEY_ESCAPE, ui::EF_NONE);
  host->OnKeyEvent(&key_up);
  hide_run_loop.Run();
  ASSERT_FALSE(host->IsVisible());
}
