// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SETTINGS_CHROME_CLEANUP_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_SETTINGS_CHROME_CLEANUP_HANDLER_H_

#include <set>

#include "base/files/file_path.h"
#include "base/macros.h"
#include "chrome/browser/safe_browsing/chrome_cleaner/chrome_cleaner_controller_win.h"
#include "chrome/browser/safe_browsing/chrome_cleaner/chrome_cleaner_scanner_results.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"

class Profile;

namespace settings {

// Chrome Cleanup settings page UI handler.
class ChromeCleanupHandler
    : public SettingsPageUIHandler,
      public safe_browsing::ChromeCleanerController::Observer {
 public:
  explicit ChromeCleanupHandler(Profile* profile);
  ~ChromeCleanupHandler() override;

  // SettingsPageUIHandler implementation.
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // ChromeCleanerController::Observer implementation.
  void OnIdle(
      safe_browsing::ChromeCleanerController::IdleReason idle_reason) override;
  void OnReporterRunning() override;
  void OnScanning() override;
  void OnInfected(bool is_powered_by_partner,
                  const safe_browsing::ChromeCleanerScannerResults&
                      reported_results) override;
  void OnCleaning(bool is_powered_by_partner,
                  const safe_browsing::ChromeCleanerScannerResults&
                      reported_results) override;
  void OnRebootRequired() override;
  void OnLogsEnabledChanged(bool logs_enabled) override;

 private:
  // Callback for the "registerChromeCleanerObserver" message. This registers
  // this object as an observer of the Chrome Cleanup global state and
  // and retrieves the current cleanup state.
  void HandleRegisterChromeCleanerObserver(const base::ListValue* args);

  // Callback for the "startScanning" message to start scanning the user's
  // system to detect unwanted software.
  void HandleStartScanning(const base::ListValue* args);

  // Callback for the "restartComputer" message to finalize the cleanup with a
  // system restart.
  void HandleRestartComputer(const base::ListValue* args);

  // Callback for the "setLogsUploadPermission" message to keep track of
  // whether the user opted-out of logs upload or not.
  void HandleSetLogsUploadPermission(const base::ListValue* args);

  // Callback for the "startCleanup" message to start removing unwanted
  // software from the user's computer.
  void HandleStartCleanup(const base::ListValue* args);

  // Callback for the "showDetails" message that notifies Chrome about whether
  // the user expanded or closed the details section of the page.
  void HandleNotifyShowDetails(const base::ListValue* args);

  // Callback for the "chromeCleanupLearnMore" message that notifies Chrome that
  // the "learn more" link was clicked.
  void HandleNotifyChromeCleanupLearnMoreClicked(const base::ListValue* args);

  // Callback for the "getMoreItemsPluralString" message, that obtains the text
  // string for the "show more" items on the detailed view.
  void HandleGetMoreItemsPluralString(const base::ListValue* args);

  // Callback for the "getItemsToRemovePluralString" message, that obtains the
  // text string for the detailed view when user-initiated cleanups are enabled.
  void HandleGetItemsToRemovePluralString(const base::ListValue* args);

  void GetPluralString(int id, const base::ListValue* args);

  // Raw pointer to a singleton. Must outlive this object.
  safe_browsing::ChromeCleanerController* controller_;

  Profile* profile_;

  DISALLOW_COPY_AND_ASSIGN(ChromeCleanupHandler);
};

}  // namespace settings

#endif  // CHROME_BROWSER_UI_WEBUI_SETTINGS_CHROME_CLEANUP_HANDLER_H_
