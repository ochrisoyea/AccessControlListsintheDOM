// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_BOOKMARKS_CELLS_BOOKMARK_TEXT_FIELD_ITEM_H_
#define IOS_CHROME_BROWSER_UI_BOOKMARKS_CELLS_BOOKMARK_TEXT_FIELD_ITEM_H_

#import <UIKit/UIKit.h>

#import "ios/chrome/browser/ui/table_view/cells/table_view_item.h"

@class BookmarkTextFieldItem;
@protocol TextFieldStyling;

// Delegates the cell's text field's events.
@protocol BookmarkTextFieldItemDelegate<UITextFieldDelegate>

// Called when the |text| of the item was changed via the textfield. The item's
// |text| is up-to-date when this is called.
- (void)textDidChangeForItem:(BookmarkTextFieldItem*)item;

@end

@interface BookmarkTextFieldItem : TableViewItem

// The text field content.
@property(nonatomic, copy) NSString* text;

// The text field placeholder.
@property(nonatomic, copy) NSString* placeholder;

// Receives the text field events.
@property(nonatomic, weak) id<BookmarkTextFieldItemDelegate> delegate;

@end

@interface LegacyBookmarkTextFieldCell : UITableViewCell

// Text field to display the title or the URL of the bookmark node.
@property(nonatomic, readonly, strong) UITextField<TextFieldStyling>* textField;

@end

#endif  // IOS_CHROME_BROWSER_UI_BOOKMARKS_CELLS_BOOKMARK_TEXT_FIELD_ITEM_H_
