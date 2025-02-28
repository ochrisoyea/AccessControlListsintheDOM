// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/browser/ui/authentication/unified_consent/unified_consent_coordinator.h"

#include "base/logging.h"
#import "ios/chrome/browser/ui/authentication/unified_consent/unified_consent_mediator.h"
#import "ios/chrome/browser/ui/authentication/unified_consent/unified_consent_view_controller.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@interface UnifiedConsentCoordinator ()<UnifiedConsentViewControllerDelegate>

@property(nonatomic, strong) UnifiedConsentMediator* unifiedConsentMediator;
@property(nonatomic, strong, readwrite)
    UnifiedConsentViewController* unifiedConsentViewController;
@property(nonatomic, readwrite) BOOL settingsLinkWasTapped;

@end

@implementation UnifiedConsentCoordinator

@synthesize delegate = _delegate;
@synthesize unifiedConsentMediator = _unifiedConsentMediator;
@synthesize unifiedConsentViewController = _unifiedConsentViewController;
@synthesize settingsLinkWasTapped = _settingsLinkWasTapped;

- (instancetype)init {
  if (self) {
    _unifiedConsentViewController = [[UnifiedConsentViewController alloc] init];
    _unifiedConsentViewController.delegate = self;
    _unifiedConsentMediator = [[UnifiedConsentMediator alloc]
        initWithUnifiedConsentViewController:_unifiedConsentViewController];
  }
  return self;
}

- (void)start {
  [self.unifiedConsentMediator start];
}

- (ChromeIdentity*)selectedIdentity {
  return self.unifiedConsentMediator.selectedIdentity;
}

- (void)setSelectedIdentity:(ChromeIdentity*)selectedIdentity {
  self.unifiedConsentMediator.selectedIdentity = selectedIdentity;
}

- (UIViewController*)viewController {
  return self.unifiedConsentViewController;
}

- (int)openSettingsStringId {
  return self.unifiedConsentViewController.openSettingsStringId;
}

- (const std::vector<int>&)consentStringIds {
  return [self.unifiedConsentViewController consentStringIds];
}

- (void)scrollToBottom {
  [self.unifiedConsentViewController scrollToBottom];
}

- (BOOL)isScrolledToBottom {
  return self.unifiedConsentViewController.isScrolledToBottom;
}

#pragma mark - UnifiedConsentViewControllerDelegate

- (void)unifiedConsentViewControllerDidTapSettingsLink:
    (UnifiedConsentViewController*)controller {
  DCHECK_EQ(self.unifiedConsentViewController, controller);
  DCHECK(!self.settingsLinkWasTapped);
  self.settingsLinkWasTapped = YES;
  [self.delegate unifiedConsentCoordinatorDidTapSettingsLink:self];
}

- (void)unifiedConsentViewControllerDidTapIdentityPickerView:
    (UnifiedConsentViewController*)controller {
  DCHECK_EQ(self.unifiedConsentViewController, controller);
  // TODO(crbug.com/827072): Needs implementation.
}

- (void)unifiedConsentViewControllerDidReachBottom:
    (UnifiedConsentViewController*)controller {
  DCHECK_EQ(self.unifiedConsentViewController, controller);
  [self.delegate unifiedConsentCoordinatorDidReachBottom:self];
}

@end
