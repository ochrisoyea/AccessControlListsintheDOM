// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BACKGROUND_BACKGROUND_CONTENTS_SERVICE_H_
#define CHROME_BROWSER_BACKGROUND_BACKGROUND_CONTENTS_SERVICE_H_

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observer.h"
#include "chrome/browser/background/background_contents.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "extensions/browser/extension_registry_observer.h"
#include "net/base/backoff_entry.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

class PrefService;
class Profile;

namespace base {
class CommandLine;
class DictionaryValue;
}

namespace content {
class SessionStorageNamespace;
}

namespace extensions {
class Extension;
class ExtensionRegistry;
}

namespace gfx {
class Rect;
}

struct BackgroundContentsOpenedDetails;

// BackgroundContentsService is owned by the profile, and is responsible for
// managing the lifetime of BackgroundContents (tracking the set of background
// urls, loading them at startup, and keeping the browser process alive as long
// as there are BackgroundContents loaded).
//
// It is also responsible for tracking the association between
// BackgroundContents and their parent app, and shutting them down when the
// parent app is unloaded.
class BackgroundContentsService : private content::NotificationObserver,
                                  public extensions::ExtensionRegistryObserver,
                                  public BackgroundContents::Delegate,
                                  public KeyedService {
 public:
  BackgroundContentsService(Profile* profile,
                            const base::CommandLine* command_line);
  ~BackgroundContentsService() override;

  // Allows tests to reduce the time between a force-installed app/extension
  // crashing and when we reload it.
  static void SetRestartDelayForForceInstalledAppsAndExtensionsForTesting(
      int restart_delay_in_ms);

  // Get the crash notification's delegate id for the extension.
  static std::string GetNotificationDelegateIdForExtensionForTesting(
      const std::string& extension_id);

  // Show a popup notification balloon with a crash message for a given app/
  // extension.
  static void ShowBalloonForTesting(const extensions::Extension* extension,
                                    Profile* profile);

  // Disable closing the crash notification balloon for tests.
  static void DisableCloseBalloonForTesting(
      bool disable_close_balloon_for_testing);

  // Returns the BackgroundContents associated with the passed application id,
  // or NULL if none.
  BackgroundContents* GetAppBackgroundContents(const std::string& appid);

  // Returns true if there's a registered BackgroundContents for this app. It
  // is possible for this routine to return true when GetAppBackgroundContents()
  // returns false, if the BackgroundContents closed due to the render process
  // crashing.
  bool HasRegisteredBackgroundContents(const std::string& appid);

  // Returns all currently opened BackgroundContents (used by the task manager).
  std::vector<BackgroundContents*> GetBackgroundContents() const;

  // BackgroundContents::Delegate implementation.
  void AddWebContents(std::unique_ptr<content::WebContents> new_contents,
                      WindowOpenDisposition disposition,
                      const gfx::Rect& initial_rect,
                      bool user_gesture,
                      bool* was_blocked) override;

  // Gets the parent application id for the passed BackgroundContents. Returns
  // an empty string if no parent application found (e.g. passed
  // BackgroundContents has already shut down).
  const std::string& GetParentApplicationId(BackgroundContents* contents) const;

  // Creates a new BackgroundContents using the passed |site| and
  // the |route_id| and begins tracking the object internally so it can be
  // shutdown if the parent application is uninstalled.
  // A BACKGROUND_CONTENTS_OPENED notification will be generated with the passed
  // |frame_name| and |application_id| values, using the passed |profile| as the
  // Source..
  BackgroundContents* CreateBackgroundContents(
      scoped_refptr<content::SiteInstance> site,
      content::RenderFrameHost* opener,
      int32_t route_id,
      int32_t main_frame_route_id,
      int32_t main_frame_widget_route_id,
      Profile* profile,
      const std::string& frame_name,
      const std::string& application_id,
      const std::string& partition_id,
      content::SessionStorageNamespace* session_storage_namespace);

  // Load the manifest-specified background page for the specified hosted app.
  // If the manifest doesn't specify one, then load the BackgroundContents
  // registered in the pref. This is typically used to reload a crashed
  // background page.
  void LoadBackgroundContentsForExtension(Profile* profile,
                                          const std::string& extension_id);

 private:
  friend class BackgroundContentsServiceTest;
  friend class MockBackgroundContents;

  FRIEND_TEST_ALL_PREFIXES(BackgroundContentsServiceTest,
                           BackgroundContentsCreateDestroy);
  FRIEND_TEST_ALL_PREFIXES(BackgroundContentsServiceTest,
                           TestApplicationIDLinkage);

  // Registers for various notifications.
  void StartObserving(Profile* profile);

  // content::NotificationObserver implementation.
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

  // Called when ExtensionSystem is ready.
  void OnExtensionSystemReady(Profile* profile);

  // extensions::ExtensionRegistryObserver implementation.
  void OnExtensionLoaded(content::BrowserContext* browser_context,
                         const extensions::Extension* extension) override;
  void OnExtensionUnloaded(content::BrowserContext* browser_context,
                           const extensions::Extension* extension,
                           extensions::UnloadedExtensionReason reason) override;
  void OnExtensionUninstalled(content::BrowserContext* browser_context,
                              const extensions::Extension* extension,
                              extensions::UninstallReason reason) override;

  // Restarts a force-installed app/extension after a crash.
  void RestartForceInstalledExtensionOnCrash(
      const extensions::Extension* extension,
      Profile* profile);

  // Loads all registered BackgroundContents at startup.
  void LoadBackgroundContentsFromPrefs(Profile* profile);

  // Load a BackgroundContent; the settings are read from the provided
  // dictionary.
  void LoadBackgroundContentsFromDictionary(
      Profile* profile,
      const std::string& extension_id,
      const base::DictionaryValue* contents);

  // Load the manifest-specified BackgroundContents for all apps for the
  // profile.
  void LoadBackgroundContentsFromManifests(Profile* profile);

  // Creates a single BackgroundContents associated with the specified |appid|,
  // creates an associated RenderView with the name specified by |frame_name|,
  // and navigates to the passed |url|.
  void LoadBackgroundContents(Profile* profile,
                              const GURL& url,
                              const std::string& frame_name,
                              const std::string& appid);

  // Invoked when a new BackgroundContents is opened.
  void BackgroundContentsOpened(BackgroundContentsOpenedDetails* details,
                                Profile* profile);

  // Invoked when a BackgroundContents object is destroyed.
  void BackgroundContentsShutdown(BackgroundContents* contents);

  // Registers the |contents->GetURL()| to be run at startup. Only happens for
  // the first navigation after window.open() (future calls to
  // RegisterBackgroundContents() for the same BackgroundContents will do
  // nothing).
  void RegisterBackgroundContents(BackgroundContents* contents);

  // Stops loading the passed BackgroundContents on startup.
  void UnregisterBackgroundContents(BackgroundContents* contents);

  // Unregisters and deletes the BackgroundContents associated with the
  // passed extension.
  void ShutdownAssociatedBackgroundContents(const std::string& appid);

  // Returns true if this BackgroundContents is in the contents_list_.
  bool IsTracked(BackgroundContents* contents) const;

  // Sends out a notification when our association of background contents with
  // apps may have changed (used by BackgroundApplicationListModel to update the
  // set of background apps as new background contents are opened/closed).
  void SendChangeNotification(Profile* profile);

  // Checks whether there has been additional |extension_id| failures. If not,
  // delete the BackoffEntry corresponding to |extension_id|, if exists.
  void MaybeClearBackoffEntry(const std::string extension_id,
                              int expected_failure_count);

  // Delay (in ms) before restarting a force-installed extension that crashed.
  static int restart_delay_in_ms_;

  // PrefService used to store list of background pages (or NULL if this is
  // running under an incognito profile).
  PrefService* prefs_;
  content::NotificationRegistrar registrar_;

  // Information we track about each BackgroundContents.
  struct BackgroundContentsInfo {
    // The BackgroundContents whose information we are tracking.
    BackgroundContents* contents;
    // The name of the top level frame for this BackgroundContents.
    std::string frame_name;
  };

  // Map associating currently loaded BackgroundContents with their parent
  // applications.
  // Key: application id
  // Value: BackgroundContentsInfo for the BC associated with that application
  typedef std::map<std::string, BackgroundContentsInfo> BackgroundContentsMap;
  BackgroundContentsMap contents_map_;

  // Map associating component extensions that have attempted to reload with a
  // BackoffEntry keeping track of retry timing.
  typedef std::map<extensions::ExtensionId, std::unique_ptr<net::BackoffEntry>>
      ComponentExtensionBackoffEntryMap;
  ComponentExtensionBackoffEntryMap component_backoff_map_;

  ScopedObserver<extensions::ExtensionRegistry,
                 extensions::ExtensionRegistryObserver>
      extension_registry_observer_;

  base::WeakPtrFactory<BackgroundContentsService> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(BackgroundContentsService);
};

#endif  // CHROME_BROWSER_BACKGROUND_BACKGROUND_CONTENTS_SERVICE_H_
