// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/lifetime/browser_close_manager.h"

#include <iterator>
#include <vector>

#include "base/stl_util.h"
#include "build/build_config.h"
#include "chrome/browser/background/background_mode_manager.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/browser_shutdown.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/notifications/notification_ui_manager.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/scoped_tabbed_browser_displayer.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/buildflags.h"
#include "content/public/browser/web_contents.h"

namespace {

// Navigates a browser window for |profile|, creating one if necessary, to the
// downloads page if there are downloads in progress for |profile|.
void ShowInProgressDownloads(Profile* profile) {
  DownloadCoreService* download_core_service =
      DownloadCoreServiceFactory::GetForBrowserContext(profile);
  if (download_core_service->NonMaliciousDownloadCount() > 0) {
    chrome::ScopedTabbedBrowserDisplayer displayer(profile);
    chrome::ShowDownloads(displayer.browser());
  }
}

}  // namespace

BrowserCloseManager::BrowserCloseManager() : current_browser_(nullptr) {
}

BrowserCloseManager::~BrowserCloseManager() {
}

void BrowserCloseManager::StartClosingBrowsers() {
  // If the session is ending, skip straight to closing the browsers. There's no
  // time to wait for beforeunload dialogs.
  if (browser_shutdown::GetShutdownType() == browser_shutdown::END_SESSION) {
    // Tell everyone that we are shutting down.
    browser_shutdown::SetTryingToQuit(true);
    CloseBrowsers();
    return;
  }
  TryToCloseBrowsers();
}

void BrowserCloseManager::CancelBrowserClose() {
  browser_shutdown::SetTryingToQuit(false);
  for (auto* browser : *BrowserList::GetInstance())
    browser->ResetTryToCloseWindow();
}

void BrowserCloseManager::TryToCloseBrowsers() {
  // If all browser windows can immediately be closed, fall out of this loop and
  // close the browsers. If any browser window cannot be closed, temporarily
  // stop closing. CallBeforeUnloadHandlers prompts the user and calls
  // OnBrowserReportCloseable with the result. If the user confirms the close,
  // this will trigger TryToCloseBrowsers to try again.
  for (auto* browser : *BrowserList::GetInstance()) {
    if (browser->TryToCloseWindow(
            false,
            base::Bind(&BrowserCloseManager::OnBrowserReportCloseable, this))) {
      current_browser_ = browser;
      return;
    }
  }
  CheckForDownloadsInProgress();
}

void BrowserCloseManager::OnBrowserReportCloseable(bool proceed) {
  if (!current_browser_)
    return;

  current_browser_ = NULL;

  if (proceed)
    TryToCloseBrowsers();
  else
    CancelBrowserClose();
}

void BrowserCloseManager::CheckForDownloadsInProgress() {
#if defined(OS_MACOSX)
  // Mac has its own in-progress downloads prompt in app_controller_mac.mm.
  CloseBrowsers();
  return;
#endif

  int download_count =
      DownloadCoreService::NonMaliciousDownloadCountAllProfiles();
  if (download_count == 0) {
    CloseBrowsers();
    return;
  }

  ConfirmCloseWithPendingDownloads(
      download_count,
      base::Bind(&BrowserCloseManager::OnReportDownloadsCancellable, this));
}

void BrowserCloseManager::ConfirmCloseWithPendingDownloads(
    int download_count,
    const base::Callback<void(bool)>& callback) {
  Browser* browser = BrowserList::GetInstance()->GetLastActive();
  DCHECK(browser);
  browser->window()->ConfirmBrowserCloseWithPendingDownloads(
      download_count,
      Browser::DOWNLOAD_CLOSE_BROWSER_SHUTDOWN,
      true,
      callback);
}

void BrowserCloseManager::OnReportDownloadsCancellable(bool proceed) {
  if (proceed) {
    CloseBrowsers();
    return;
  }

  CancelBrowserClose();

  // Open the downloads page for each profile with downloads in progress.
  std::vector<Profile*> profiles(
      g_browser_process->profile_manager()->GetLoadedProfiles());
  for (Profile* profile : profiles) {
    ShowInProgressDownloads(profile);
    if (profile->HasOffTheRecordProfile())
      ShowInProgressDownloads(profile->GetOffTheRecordProfile());
  }
}

void BrowserCloseManager::CloseBrowsers() {
#if BUILDFLAG(ENABLE_SESSION_SERVICE)
  // Before we close the browsers shutdown all session services. That way an
  // exit can restore all browsers open before exiting.
  ProfileManager::ShutdownSessionServices();
#endif
  if (!browser_shutdown::IsTryingToQuit()) {
    BackgroundModeManager* background_mode_manager =
        g_browser_process->background_mode_manager();
    if (background_mode_manager)
      background_mode_manager->SuspendBackgroundMode();
  }

  // Make a copy of the BrowserList to simplify the case where we need to
  // destroy a Browser during the loop.
  std::vector<Browser*> browser_list_copy;
  std::copy(BrowserList::GetInstance()->begin(),
            BrowserList::GetInstance()->end(),
            std::back_inserter(browser_list_copy));

  bool session_ending =
      browser_shutdown::GetShutdownType() == browser_shutdown::END_SESSION;

  for (auto* browser : browser_list_copy) {
    browser->window()->Close();
    if (session_ending) {
      // This path is hit during logoff/power-down. In this case we won't get
      // a final message and so we force the browser to be deleted.
      // Close doesn't immediately destroy the browser
      // (Browser::TabStripEmpty() uses invoke later) but when we're ending the
      // session we need to make sure the browser is destroyed now. So, invoke
      // DestroyBrowser to make sure the browser is deleted and cleanup can
      // happen.
      while (browser->tab_strip_model()->count())
        browser->tab_strip_model()->DetachWebContentsAt(0);
      browser->window()->DestroyBrowser();
      // Destroying the browser should have removed it from the browser list.
      DCHECK(!base::ContainsValue(*BrowserList::GetInstance(), browser));
    }
  }

  NotificationUIManager* notification_manager =
      g_browser_process->notification_ui_manager();
  if (notification_manager)
    notification_manager->CancelAll();
}
