// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/bookmarks/cells/bookmark_parent_folder_item.h"

#include "base/mac/foundation_util.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_utils_ios.h"
#import "ios/chrome/browser/ui/icons/chrome_icon.h"
#import "ios/chrome/browser/ui/uikit_ui_util.h"
#include "ios/chrome/grit/ios_strings.h"
#import "ios/third_party/material_components_ios/src/components/Typography/src/MaterialTypography.h"
#include "ui/base/l10n/l10n_util_mac.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@implementation BookmarkParentFolderItem

@synthesize title = _title;

- (instancetype)initWithType:(NSInteger)type {
  self = [super initWithType:type];
  if (self) {
    self.accessibilityIdentifier = @"Change Folder";
    self.cellClass = [LegacyBookmarkParentFolderCell class];
  }
  return self;
}

#pragma mark TableViewItem

- (void)configureCell:(UITableViewCell*)tableCell
           withStyler:(ChromeTableViewStyler*)styler {
  [super configureCell:tableCell withStyler:styler];
  LegacyBookmarkParentFolderCell* cell =
      base::mac::ObjCCastStrict<LegacyBookmarkParentFolderCell>(tableCell);
  cell.parentFolderNameLabel.text = self.title;
}

@end

@interface LegacyBookmarkParentFolderCell ()
@property(nonatomic, readwrite, strong) UILabel* parentFolderNameLabel;
@property(nonatomic, strong) UILabel* decorationLabel;
@end

@implementation LegacyBookmarkParentFolderCell

@synthesize parentFolderNameLabel = _parentFolderNameLabel;
@synthesize decorationLabel = _decorationLabel;

- (instancetype)initWithStyle:(UITableViewCellStyle)style
              reuseIdentifier:(NSString*)reuseIdentifier {
  self = [super initWithStyle:style reuseIdentifier:reuseIdentifier];
  if (!self)
    return nil;

  self.isAccessibilityElement = YES;
  self.accessibilityTraits |= UIAccessibilityTraitButton;

  const CGFloat kHorizontalPadding = 15;
  const CGFloat kVerticalPadding = 8;
  const CGFloat kParentFolderLabelTopPadding = 7;

  UIView* containerView = [[UIView alloc] initWithFrame:CGRectZero];
  containerView.translatesAutoresizingMaskIntoConstraints = NO;

  _decorationLabel = [[UILabel alloc] init];
  _decorationLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _decorationLabel.text = l10n_util::GetNSString(IDS_IOS_BOOKMARK_GROUP_BUTTON);
  _decorationLabel.font = [[MDCTypography fontLoader] regularFontOfSize:12];
  _decorationLabel.textColor = bookmark_utils_ios::lightTextColor();
  [containerView addSubview:_decorationLabel];

  _parentFolderNameLabel = [[UILabel alloc] init];
  _parentFolderNameLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _parentFolderNameLabel.font =
      [[MDCTypography fontLoader] regularFontOfSize:16];
  _parentFolderNameLabel.textColor =
      [UIColor colorWithWhite:33.0 / 255.0 alpha:1.0];
  _parentFolderNameLabel.textAlignment = NSTextAlignmentNatural;
  [containerView addSubview:_parentFolderNameLabel];

  UIImageView* navigationChevronImage = [[UIImageView alloc] init];
  UIImage* image = TintImage([ChromeIcon chevronIcon], [UIColor grayColor]);
  navigationChevronImage.image = image;
  navigationChevronImage.translatesAutoresizingMaskIntoConstraints = NO;
  [containerView addSubview:navigationChevronImage];

  [self.contentView addSubview:containerView];

  // Set up the constraints.
  [NSLayoutConstraint activateConstraints:@[
    [_decorationLabel.topAnchor
        constraintEqualToAnchor:containerView.topAnchor],
    [_decorationLabel.leadingAnchor
        constraintEqualToAnchor:containerView.leadingAnchor],
    [_parentFolderNameLabel.topAnchor
        constraintEqualToAnchor:_decorationLabel.bottomAnchor
                       constant:kParentFolderLabelTopPadding],
    [_parentFolderNameLabel.leadingAnchor
        constraintEqualToAnchor:_decorationLabel.leadingAnchor],
    [_parentFolderNameLabel.bottomAnchor
        constraintEqualToAnchor:containerView.bottomAnchor],
    [navigationChevronImage.centerYAnchor
        constraintEqualToAnchor:_parentFolderNameLabel.centerYAnchor],
    [navigationChevronImage.leadingAnchor
        constraintEqualToAnchor:_parentFolderNameLabel.trailingAnchor],
    [navigationChevronImage.widthAnchor
        constraintEqualToConstant:navigationChevronImage.image.size.width],
    [navigationChevronImage.trailingAnchor
        constraintEqualToAnchor:containerView.trailingAnchor],
    [containerView.leadingAnchor
        constraintEqualToAnchor:self.contentView.leadingAnchor
                       constant:kHorizontalPadding],
    [containerView.trailingAnchor
        constraintEqualToAnchor:self.contentView.trailingAnchor
                       constant:-kHorizontalPadding],
    [containerView.topAnchor constraintEqualToAnchor:self.contentView.topAnchor
                                            constant:kVerticalPadding],
    [containerView.bottomAnchor
        constraintEqualToAnchor:self.contentView.bottomAnchor
                       constant:-kVerticalPadding],
  ]];

  return self;
}

- (void)prepareForReuse {
  [super prepareForReuse];
  self.parentFolderNameLabel.text = nil;
}

- (NSString*)accessibilityLabel {
  return self.parentFolderNameLabel.text;
}

- (NSString*)accessibilityHint {
  return l10n_util::GetNSString(
      IDS_IOS_BOOKMARK_EDIT_PARENT_FOLDER_BUTTON_HINT);
}

@end
