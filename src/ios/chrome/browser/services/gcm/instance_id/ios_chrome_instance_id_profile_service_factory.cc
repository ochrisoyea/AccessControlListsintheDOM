// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/browser/services/gcm/instance_id/ios_chrome_instance_id_profile_service_factory.h"

#include "base/memory/ptr_util.h"
#include "base/memory/singleton.h"
#include "components/gcm_driver/gcm_profile_service.h"
#include "components/gcm_driver/instance_id/instance_id_profile_service.h"
#include "components/keyed_service/ios/browser_state_dependency_manager.h"
#include "ios/chrome/browser/browser_state/chrome_browser_state.h"
#include "ios/chrome/browser/services/gcm/ios_chrome_gcm_profile_service_factory.h"

// static
instance_id::InstanceIDProfileService*
IOSChromeInstanceIDProfileServiceFactory::GetForBrowserState(
    ios::ChromeBrowserState* browser_state) {
  return static_cast<instance_id::InstanceIDProfileService*>(
      GetInstance()->GetServiceForBrowserState(browser_state, true));
}

// static
IOSChromeInstanceIDProfileServiceFactory*
IOSChromeInstanceIDProfileServiceFactory::GetInstance() {
  return base::Singleton<IOSChromeInstanceIDProfileServiceFactory>::get();
}

IOSChromeInstanceIDProfileServiceFactory::
    IOSChromeInstanceIDProfileServiceFactory()
    : BrowserStateKeyedServiceFactory(
          "InstanceIDProfileService",
          BrowserStateDependencyManager::GetInstance()) {
  DependsOn(IOSChromeGCMProfileServiceFactory::GetInstance());
}

IOSChromeInstanceIDProfileServiceFactory::
    ~IOSChromeInstanceIDProfileServiceFactory() {}

std::unique_ptr<KeyedService>
IOSChromeInstanceIDProfileServiceFactory::BuildServiceInstanceFor(
    web::BrowserState* context) const {
  DCHECK(!context->IsOffTheRecord());

  ios::ChromeBrowserState* browser_state =
      ios::ChromeBrowserState::FromBrowserState(context);
  return std::make_unique<instance_id::InstanceIDProfileService>(
      IOSChromeGCMProfileServiceFactory::GetForBrowserState(browser_state)
          ->driver(),
      browser_state->IsOffTheRecord());
}
