// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BACKGROUND_BACKGROUND_CONTENTS_H_
#define CHROME_BROWSER_BACKGROUND_BACKGROUND_CONTENTS_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/macros.h"
#include "base/observer_list.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/deferred_start_render_host.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

class Profile;

namespace content {
class SessionStorageNamespace;
};

namespace extensions {
class ExtensionHostDelegate;
}

// This class maintains a WebContents used in the background. It can host a
// renderer, but does not have any visible display.
// TODO(atwilson): Unify this with background pages; http://crbug.com/77790
class BackgroundContents : public extensions::DeferredStartRenderHost,
                           public content::WebContentsDelegate,
                           public content::WebContentsObserver,
                           public content::NotificationObserver {
 public:
  class Delegate {
   public:
    // Called by AddNewContents(). Asks the delegate to attach the opened
    // WebContents to a suitable container (e.g. browser) or to show it if it's
    // a popup window. If |was_blocked| is non-NULL, then |*was_blocked| will be
    // set to true if the popup gets blocked, and left unchanged otherwise.
    virtual void AddWebContents(
        std::unique_ptr<content::WebContents> new_contents,
        WindowOpenDisposition disposition,
        const gfx::Rect& initial_rect,
        bool user_gesture,
        bool* was_blocked) = 0;

   protected:
    virtual ~Delegate() {}
  };

  BackgroundContents(
      scoped_refptr<content::SiteInstance> site_instance,
      content::RenderFrameHost* opener,
      int32_t routing_id,
      int32_t main_frame_routing_id,
      int32_t main_frame_widget_routing_id,
      Delegate* delegate,
      const std::string& partition_id,
      content::SessionStorageNamespace* session_storage_namespace);
  ~BackgroundContents() override;

  content::WebContents* web_contents() const { return web_contents_.get(); }
  virtual const GURL& GetURL() const;

  // Adds this BackgroundContents to the queue of RenderViews to create.
  void CreateRenderViewSoon(const GURL& url);

  // content::WebContentsDelegate implementation:
  void CloseContents(content::WebContents* source) override;
  bool ShouldSuppressDialogs(content::WebContents* source) override;
  void DidNavigateMainFramePostCommit(content::WebContents* tab) override;
  void AddNewContents(content::WebContents* source,
                      std::unique_ptr<content::WebContents> new_contents,
                      WindowOpenDisposition disposition,
                      const gfx::Rect& initial_rect,
                      bool user_gesture,
                      bool* was_blocked) override;
  bool IsNeverVisible(content::WebContents* web_contents) override;

  // content::WebContentsObserver implementation:
  void RenderProcessGone(base::TerminationStatus status) override;
  void DidStartLoading() override;
  void DidStopLoading() override;

  // content::NotificationObserver
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

 protected:
  // Exposed for testing.
  BackgroundContents();

 private:
  // extensions::DeferredStartRenderHost implementation:
  void CreateRenderViewNow() override;
  void AddDeferredStartRenderHostObserver(
      extensions::DeferredStartRenderHostObserver* observer) override;
  void RemoveDeferredStartRenderHostObserver(
      extensions::DeferredStartRenderHostObserver* observer) override;

  // The delegate for this BackgroundContents.
  Delegate* delegate_;

  // Delegate for choosing an ExtensionHostQueue.
  std::unique_ptr<extensions::ExtensionHostDelegate> extension_host_delegate_;

  Profile* profile_;
  std::unique_ptr<content::WebContents> web_contents_;
  content::NotificationRegistrar registrar_;
  base::ObserverList<extensions::DeferredStartRenderHostObserver>
      deferred_start_render_host_observer_list_;

  // The initial URL to load.
  GURL initial_url_;

  DISALLOW_COPY_AND_ASSIGN(BackgroundContents);
};

// This is the data sent out as the details with BACKGROUND_CONTENTS_OPENED.
struct BackgroundContentsOpenedDetails {
  // The BackgroundContents object that has just been opened.
  BackgroundContents* contents;

  // The name of the parent frame for these contents.
  const std::string& frame_name;

  // The ID of the parent application (if any).
  const std::string& application_id;
};

#endif  // CHROME_BROWSER_BACKGROUND_BACKGROUND_CONTENTS_H_
