// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/message_center/views/notification_view_md.h"

#include <stddef.h>
#include <memory>

#include "base/i18n/case_conversion.h"
#include "base/strings/string_util.h"
#include "components/url_formatter/elide_url.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/gesture_detection/gesture_provider_config_helper.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/skia_util.h"
#include "ui/gfx/text_elider.h"
#include "ui/message_center/message_center.h"
#include "ui/message_center/public/cpp/message_center_constants.h"
#include "ui/message_center/public/cpp/notification.h"
#include "ui/message_center/public/cpp/notification_types.h"
#include "ui/message_center/vector_icons.h"
#include "ui/message_center/views/bounded_label.h"
#include "ui/message_center/views/notification_control_buttons_view.h"
#include "ui/message_center/views/notification_header_view.h"
#include "ui/message_center/views/padded_button.h"
#include "ui/message_center/views/proportional_image_view.h"
#include "ui/strings/grit/ui_strings.h"
#include "ui/views/animation/flood_fill_ink_drop_ripple.h"
#include "ui/views/animation/ink_drop_highlight.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/radio_button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/progress_bar.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/native_cursor.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace message_center {

namespace {

// Dimensions.
constexpr gfx::Insets kContentRowPadding(0, 12, 16, 12);
constexpr gfx::Insets kActionsRowPadding(8, 8, 8, 8);
constexpr int kActionsRowHorizontalSpacing = 8;
constexpr gfx::Insets kActionButtonPadding(0, 12, 0, 12);
constexpr gfx::Insets kStatusTextPadding(4, 0, 0, 0);
constexpr gfx::Size kActionButtonMinSize(0, 32);
// TODO(tetsui): Move |kIconViewSize| to public/cpp/message_center_constants.h
// and merge with contradicting |kNotificationIconSize|.
constexpr gfx::Size kIconViewSize(36, 36);
constexpr gfx::Insets kLargeImageContainerPadding(0, 12, 12, 12);
constexpr gfx::Size kLargeImageMinSize(328, 0);
constexpr gfx::Size kLargeImageMaxSize(328, 218);
constexpr gfx::Insets kLeftContentPadding(2, 4, 0, 4);
constexpr gfx::Insets kLeftContentPaddingWithIcon(2, 4, 0, 12);
constexpr gfx::Insets kInputTextfieldPadding(16, 16, 16, 0);
constexpr gfx::Insets kInputReplyButtonPadding(0, 14, 0, 14);
constexpr gfx::Insets kSettingsRowPadding(8, 0, 0, 0);
constexpr gfx::Insets kSettingsRadioButtonPadding(14, 18, 14, 18);
constexpr gfx::Insets kSettingsButtonRowPadding(8);

// Background of inline actions area.
constexpr SkColor kActionsRowBackgroundColor = SkColorSetRGB(0xee, 0xee, 0xee);
// Ripple ink drop opacity of action buttons.
const float kActionButtonInkDropRippleVisibleOpacity = 0.08f;
// Highlight (hover) ink drop opacity of action buttons.
const float kActionButtonInkDropHighlightVisibleOpacity = 0.08f;
// Text color of action button.
constexpr SkColor kActionButtonTextColor = gfx::kGoogleBlue700;
// Background color of the large image.
constexpr SkColor kLargeImageBackgroundColor = SkColorSetRGB(0xf5, 0xf5, 0xf5);

constexpr SkColor kRegularTextColorMD = SkColorSetRGB(0x21, 0x21, 0x21);
constexpr SkColor kDimTextColorMD = SkColorSetRGB(0x75, 0x75, 0x75);

// Background of inline settings area.
const SkColor kSettingsRowBackgroundColor = SkColorSetRGB(0xee, 0xee, 0xee);

// Text color and icon color of inline reply area when the textfield is empty.
constexpr SkColor kTextfieldPlaceholderTextColorMD =
    SkColorSetA(SK_ColorWHITE, 0x8A);
constexpr SkColor kTextfieldPlaceholderIconColorMD =
    SkColorSetA(SK_ColorWHITE, 0x60);

// The icon size of inline reply input field.
constexpr int kInputReplyButtonSize = 20;

// Max number of lines for message_view_.
constexpr int kMaxLinesForMessageView = 1;
constexpr int kMaxLinesForExpandedMessageView = 4;

constexpr int kCompactTitleMessageViewSpacing = 12;

constexpr int kProgressBarHeight = 4;

constexpr int kMessageViewWidthWithIcon =
    kNotificationWidth - kIconViewSize.width() -
    kLeftContentPaddingWithIcon.left() - kLeftContentPaddingWithIcon.right() -
    kContentRowPadding.left() - kContentRowPadding.right();

constexpr int kMessageViewWidth =
    kNotificationWidth - kLeftContentPadding.left() -
    kLeftContentPadding.right() - kContentRowPadding.left() -
    kContentRowPadding.right();

const int kMinPixelsPerTitleCharacterMD = 4;

// Character limit = pixels per line * line limit / min. pixels per character.
constexpr size_t kMessageCharacterLimitMD =
    kNotificationWidth * kMessageExpandedLineLimit / 3;

// The default is 12, so this normally come out to 13.
constexpr int kTextFontSizeDelta = 1;

// In progress notification, if both the title and the message are long, the
// message would be prioritized and the title would be elided.
// However, it is not perferable that we completely omit the title, so
// the ratio of the message width is limited to this value.
constexpr double kProgressNotificationMessageRatio = 0.7;

// Users on ChromeOS are used to the Settings and Close buttons not being
// visible at all times, but users on other platforms expect them to be visible.
constexpr bool AlwaysShowControlButtons() {
#if defined(OS_CHROMEOS)
  return false;
#else
  return true;
#endif
}

// FontList for the texts except for the header.
gfx::FontList GetTextFontList() {
  gfx::Font default_font;
  gfx::Font font = default_font.Derive(kTextFontSizeDelta, gfx::Font::NORMAL,
                                       gfx::Font::Weight::NORMAL);
  return gfx::FontList(font);
}

class ClickActivator : public ui::EventHandler {
 public:
  explicit ClickActivator(NotificationViewMD* owner) : owner_(owner) {}
  ~ClickActivator() override = default;

 private:
  // ui::EventHandler
  void OnEvent(ui::Event* event) override {
    if (event->type() == ui::ET_MOUSE_PRESSED ||
        event->type() == ui::ET_GESTURE_TAP) {
      owner_->Activate();
    }
  }

  NotificationViewMD* const owner_;

  DISALLOW_COPY_AND_ASSIGN(ClickActivator);
};

}  // anonymous namespace

// ItemView ////////////////////////////////////////////////////////////////////

ItemView::ItemView(const NotificationItem& item) {
  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kHorizontal, gfx::Insets(), 0));

  const gfx::FontList font_list = GetTextFontList();

  views::Label* title = new views::Label(item.title);
  title->SetFontList(font_list);
  title->set_collapse_when_hidden(true);
  title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  title->SetEnabledColor(kRegularTextColorMD);
  title->SetAutoColorReadabilityEnabled(false);
  AddChildView(title);

  views::Label* message = new views::Label(l10n_util::GetStringFUTF16(
      IDS_MESSAGE_CENTER_LIST_NOTIFICATION_MESSAGE_WITH_DIVIDER, item.message));
  message->SetFontList(font_list);
  message->set_collapse_when_hidden(true);
  message->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  message->SetEnabledColor(kDimTextColorMD);
  message->SetAutoColorReadabilityEnabled(false);
  AddChildView(message);
}

ItemView::~ItemView() = default;

const char* ItemView::GetClassName() const {
  return "ItemView";
}

// CompactTitleMessageView /////////////////////////////////////////////////////

CompactTitleMessageView::~CompactTitleMessageView() = default;

const char* CompactTitleMessageView::GetClassName() const {
  return "CompactTitleMessageView";
}

CompactTitleMessageView::CompactTitleMessageView() {
  const gfx::FontList& font_list = GetTextFontList();

  title_ = new views::Label();
  title_->SetFontList(font_list);
  title_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  title_->SetEnabledColor(kRegularTextColorMD);
  AddChildView(title_);

  message_ = new views::Label();
  message_->SetFontList(font_list);
  message_->SetHorizontalAlignment(gfx::ALIGN_RIGHT);
  message_->SetEnabledColor(kDimTextColorMD);
  AddChildView(message_);
}

gfx::Size CompactTitleMessageView::CalculatePreferredSize() const {
  gfx::Size title_size = title_->GetPreferredSize();
  gfx::Size message_size = message_->GetPreferredSize();
  return gfx::Size(title_size.width() + message_size.width() +
                       kCompactTitleMessageViewSpacing,
                   std::max(title_size.height(), message_size.height()));
}

void CompactTitleMessageView::Layout() {
  // Elides title and message.
  // * If the message is too long, the message occupies at most
  //   kProgressNotificationMessageRatio of the width.
  // * If the title is too long, the full content of the message is shown,
  //   kCompactTitleMessageViewSpacing is added between them, and the elided
  //   title is shown.
  // * If they are short enough, the title is left-aligned and the message is
  //   right-aligned.
  const int message_width = std::min(
      message_->GetPreferredSize().width(),
      title_->GetPreferredSize().width() > 0
          ? static_cast<int>(kProgressNotificationMessageRatio * width())
          : width());
  const int title_width =
      std::max(0, width() - message_width - kCompactTitleMessageViewSpacing);

  title_->SetBounds(0, 0, title_width, height());
  message_->SetBounds(width() - message_width, 0, message_width, height());
}

void CompactTitleMessageView::set_title(const base::string16& title) {
  title_->SetText(title);
}

void CompactTitleMessageView::set_message(const base::string16& message) {
  message_->SetText(message);
}

// LargeImageView //////////////////////////////////////////////////////////////

LargeImageView::LargeImageView() {
  SetBackground(views::CreateSolidBackground(kLargeImageBackgroundColor));
}

LargeImageView::~LargeImageView() = default;

void LargeImageView::SetImage(const gfx::ImageSkia& image) {
  image_ = image;
  gfx::Size preferred_size = GetResizedImageSize();
  preferred_size.SetToMax(kLargeImageMinSize);
  preferred_size.SetToMin(kLargeImageMaxSize);
  SetPreferredSize(preferred_size);
  SchedulePaint();
  Layout();
}

void LargeImageView::OnPaint(gfx::Canvas* canvas) {
  views::View::OnPaint(canvas);

  gfx::Size resized_size = GetResizedImageSize();
  gfx::Size drawn_size = resized_size;
  drawn_size.SetToMin(kLargeImageMaxSize);
  gfx::Rect drawn_bounds = GetContentsBounds();
  drawn_bounds.ClampToCenteredSize(drawn_size);

  gfx::ImageSkia resized_image = gfx::ImageSkiaOperations::CreateResizedImage(
      image_, skia::ImageOperations::RESIZE_BEST, resized_size);

  // Cut off the overflown part.
  gfx::ImageSkia drawn_image = gfx::ImageSkiaOperations::ExtractSubset(
      resized_image, gfx::Rect(drawn_size));

  canvas->DrawImageInt(drawn_image, drawn_bounds.x(), drawn_bounds.y());
}

const char* LargeImageView::GetClassName() const {
  return "LargeImageView";
}

// Returns expected size of the image right after resizing.
// The GetResizedImageSize().width() <= kLargeImageMaxSize.width() holds, but
// GetResizedImageSize().height() may be larger than kLargeImageMaxSize.height()
// In this case, the overflown part will be just cutted off from the view.
gfx::Size LargeImageView::GetResizedImageSize() {
  gfx::Size original_size = image_.size();
  if (original_size.width() <= kLargeImageMaxSize.width())
    return image_.size();

  const double proportion =
      original_size.height() / static_cast<double>(original_size.width());
  gfx::Size resized_size;
  resized_size.SetSize(kLargeImageMaxSize.width(),
                       kLargeImageMaxSize.width() * proportion);
  return resized_size;
}

// LargeImageContainerView /////////////////////////////////////////////////////

LargeImageContainerView::LargeImageContainerView()
    : image_view_(new LargeImageView()) {
  SetLayoutManager(std::make_unique<views::FillLayout>());
  SetBorder(views::CreateEmptyBorder(kLargeImageContainerPadding));
  SetBackground(views::CreateSolidBackground(kImageBackgroundColor));
  AddChildView(image_view_);
}

LargeImageContainerView::~LargeImageContainerView() = default;

void LargeImageContainerView::SetImage(const gfx::ImageSkia& image) {
  image_view_->SetImage(image);
}

const char* LargeImageContainerView::GetClassName() const {
  return "LargeImageContainerView";
}

// NotificationButtonMD ////////////////////////////////////////////////////////

NotificationButtonMD::NotificationButtonMD(
    views::ButtonListener* listener,
    const base::string16& label,
    const base::Optional<base::string16>& placeholder)
    : views::LabelButton(listener,
                         base::i18n::ToUpper(label),
                         views::style::CONTEXT_BUTTON_MD),
      placeholder_(placeholder) {
  SetHorizontalAlignment(gfx::ALIGN_CENTER);
  SetInkDropMode(views::LabelButton::InkDropMode::ON);
  set_has_ink_drop_action_on_click(true);
  set_ink_drop_base_color(SK_ColorBLACK);
  set_ink_drop_visible_opacity(kActionButtonInkDropRippleVisibleOpacity);
  SetEnabledTextColors(kActionButtonTextColor);
  SetBorder(views::CreateEmptyBorder(kActionButtonPadding));
  SetMinSize(kActionButtonMinSize);
  SetFocusForPlatform();
}

NotificationButtonMD::~NotificationButtonMD() = default;

void NotificationButtonMD::SetText(const base::string16& text) {
  views::LabelButton::SetText(base::i18n::ToUpper(text));
}

const char* NotificationButtonMD::GetClassName() const {
  return "NotificationButtonMD";
}

std::unique_ptr<views::InkDropHighlight>
NotificationButtonMD::CreateInkDropHighlight() const {
  std::unique_ptr<views::InkDropHighlight> highlight =
      views::LabelButton::CreateInkDropHighlight();
  highlight->set_visible_opacity(kActionButtonInkDropHighlightVisibleOpacity);
  return highlight;
}

// NotificationInputTextfieldMD ////////////////////////////////////////////////

NotificationInputTextfieldMD::NotificationInputTextfieldMD(
    views::TextfieldController* controller)
    : index_(0) {
  set_controller(controller);
  SetTextColor(SK_ColorWHITE);
  SetBackgroundColor(SK_ColorTRANSPARENT);
  set_placeholder_text_color(kTextfieldPlaceholderTextColorMD);
  SetBorder(views::CreateEmptyBorder(kInputTextfieldPadding));
}

NotificationInputTextfieldMD::~NotificationInputTextfieldMD() = default;

void NotificationInputTextfieldMD::set_placeholder(
    const base::string16& placeholder) {
  if (placeholder.empty()) {
    set_placeholder_text(l10n_util::GetStringUTF16(
        IDS_MESSAGE_CENTER_NOTIFICATION_INLINE_REPLY_PLACEHOLDER));
  } else {
    set_placeholder_text(placeholder);
  }
}

// NotificationInputReplyButtonMD //////////////////////////////////////////////

NotificationInputReplyButtonMD::NotificationInputReplyButtonMD(
    views::ButtonListener* listener)
    : views::ImageButton(listener) {
  SetPlaceholderImage();
  SetBorder(views::CreateEmptyBorder(kInputReplyButtonPadding));
  SetImageAlignment(ALIGN_CENTER, ALIGN_MIDDLE);
}

NotificationInputReplyButtonMD::~NotificationInputReplyButtonMD() = default;

void NotificationInputReplyButtonMD::SetNormalImage() {
  SetImage(STATE_NORMAL,
           gfx::CreateVectorIcon(kNotificationInlineReplyIcon,
                                 kInputReplyButtonSize, SK_ColorWHITE));
}

void NotificationInputReplyButtonMD::SetPlaceholderImage() {
  SetImage(
      STATE_NORMAL,
      gfx::CreateVectorIcon(kNotificationInlineReplyIcon, kInputReplyButtonSize,
                            kTextfieldPlaceholderIconColorMD));
}

// NotificationInputContainerMD ////////////////////////////////////////////////

NotificationInputContainerMD::NotificationInputContainerMD(
    NotificationInputDelegate* delegate)
    : delegate_(delegate),
      ink_drop_container_(new views::InkDropContainerView()),
      textfield_(new NotificationInputTextfieldMD(this)),
      button_(new NotificationInputReplyButtonMD(this)) {
  auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kHorizontal, gfx::Insets(), 0));
  SetBackground(views::CreateSolidBackground(kActionsRowBackgroundColor));

  SetInkDropMode(InkDropMode::ON);
  set_ink_drop_visible_opacity(1);

  ink_drop_container_->SetPaintToLayer();
  ink_drop_container_->layer()->SetFillsBoundsOpaquely(false);
  AddChildView(ink_drop_container_);

  AddChildView(textfield_);
  layout->SetFlexForView(textfield_, 1);

  AddChildView(button_);
}

NotificationInputContainerMD::~NotificationInputContainerMD() = default;

void NotificationInputContainerMD::AnimateBackground(
    const ui::LocatedEvent& event) {
  if (View::HitTestPoint(event.location()))
    AnimateInkDrop(views::InkDropState::ACTION_PENDING,
                   ui::LocatedEvent::FromIfValid(&event));
}

void NotificationInputContainerMD::AddInkDropLayer(ui::Layer* ink_drop_layer) {
  textfield_->SetPaintToLayer();
  textfield_->layer()->SetFillsBoundsOpaquely(false);
  button_->SetPaintToLayer();
  button_->layer()->SetFillsBoundsOpaquely(false);
  ink_drop_container_->AddInkDropLayer(ink_drop_layer);
  InstallInkDropMask(ink_drop_layer);
}

void NotificationInputContainerMD::RemoveInkDropLayer(
    ui::Layer* ink_drop_layer) {
  textfield_->DestroyLayer();
  button_->DestroyLayer();
  ResetInkDropMask();
  ink_drop_container_->RemoveInkDropLayer(ink_drop_layer);
}

std::unique_ptr<views::InkDropRipple>
NotificationInputContainerMD::CreateInkDropRipple() const {
  return std::make_unique<views::FloodFillInkDropRipple>(
      size(), GetInkDropCenterBasedOnLastEvent(), GetInkDropBaseColor(),
      ink_drop_visible_opacity());
}

SkColor NotificationInputContainerMD::GetInkDropBaseColor() const {
  return gfx::kGoogleBlue700;
}

bool NotificationInputContainerMD::HandleKeyEvent(views::Textfield* sender,
                                                  const ui::KeyEvent& event) {
  if (event.type() == ui::ET_KEY_PRESSED &&
      event.key_code() == ui::VKEY_RETURN) {
    delegate_->OnNotificationInputSubmit(textfield_->index(),
                                         textfield_->text());
    return true;
  }
  return event.type() == ui::ET_KEY_RELEASED;
}

void NotificationInputContainerMD::OnAfterUserAction(views::Textfield* sender) {
  if (textfield_->text().empty()) {
    button_->SetPlaceholderImage();
  } else {
    button_->SetNormalImage();
  }
}

void NotificationInputContainerMD::ButtonPressed(views::Button* sender,
                                                 const ui::Event& event) {
  if (sender == button_) {
    delegate_->OnNotificationInputSubmit(textfield_->index(),
                                         textfield_->text());
  }
}

// InlineSettingsRadioButton ///////////////////////////////////////////////////

class InlineSettingsRadioButton : public views::RadioButton {
 public:
  InlineSettingsRadioButton(const base::string16& label_text)
      : views::RadioButton(label_text, 1 /* group */, true /* force_md */) {
    label()->SetFontList(GetTextFontList());
    label()->SetEnabledColor(kRegularTextColorMD);
    label()->SetSubpixelRenderingEnabled(false);
  }
};

// ////////////////////////////////////////////////////////////
// NotificationViewMD
// ////////////////////////////////////////////////////////////

void NotificationViewMD::CreateOrUpdateViews(const Notification& notification) {
  CreateOrUpdateContextTitleView(notification);
  CreateOrUpdateTitleView(notification);
  CreateOrUpdateMessageView(notification);
  CreateOrUpdateCompactTitleMessageView(notification);
  CreateOrUpdateProgressBarView(notification);
  CreateOrUpdateProgressStatusView(notification);
  CreateOrUpdateListItemViews(notification);
  CreateOrUpdateIconView(notification);
  CreateOrUpdateSmallIconView(notification);
  CreateOrUpdateImageView(notification);
  CreateOrUpdateInlineSettingsViews(notification);
  UpdateViewForExpandedState(expanded_);
  // Should be called at the last because SynthesizeMouseMoveEvent() requires
  // everything is in the right location when called.
  CreateOrUpdateActionButtonViews(notification);
}

NotificationViewMD::NotificationViewMD(const Notification& notification)
    : MessageView(notification),
      ink_drop_container_(new views::InkDropContainerView()) {
  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kVertical, gfx::Insets(), 0));

  set_ink_drop_visible_opacity(1);

  ink_drop_container_->SetPaintToLayer();
  ink_drop_container_->layer()->SetFillsBoundsOpaquely(false);
  AddChildView(ink_drop_container_);

  control_buttons_view_ =
      std::make_unique<NotificationControlButtonsView>(this);
  control_buttons_view_->set_owned_by_client();
  control_buttons_view_->SetBackgroundColor(SK_ColorTRANSPARENT);

  // |header_row_| contains app_icon, app_name, control buttons, etc...
  header_row_ = new NotificationHeaderView(control_buttons_view_.get(), this);
  AddChildView(header_row_);

  // |content_row_| contains title, message, image, progressbar, etc...
  content_row_ = new views::View();
  auto* content_row_layout =
      content_row_->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::kHorizontal, kContentRowPadding, 0));
  content_row_layout->set_cross_axis_alignment(
      views::BoxLayout::CROSS_AXIS_ALIGNMENT_START);
  AddChildView(content_row_);

  // |left_content_| contains most contents like title, message, etc...
  left_content_ = new views::View();
  left_content_->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kVertical, gfx::Insets(), 0));
  left_content_->SetBorder(views::CreateEmptyBorder(kLeftContentPadding));
  content_row_->AddChildView(left_content_);
  content_row_layout->SetFlexForView(left_content_, 1);

  // |right_content_| contains notification icon and small image.
  right_content_ = new views::View();
  right_content_->SetLayoutManager(std::make_unique<views::FillLayout>());
  content_row_->AddChildView(right_content_);

  // |action_row_| contains inline action buttons and inline textfield.
  actions_row_ = new views::View();
  actions_row_->SetVisible(false);
  actions_row_->SetLayoutManager(std::make_unique<views::FillLayout>());
  AddChildView(actions_row_);

  // |action_buttons_row_| contains inline action buttons.
  action_buttons_row_ = new views::View();
  action_buttons_row_->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kHorizontal, kActionsRowPadding,
      kActionsRowHorizontalSpacing));
  action_buttons_row_->SetBackground(
      views::CreateSolidBackground(kActionsRowBackgroundColor));
  action_buttons_row_->SetVisible(false);
  actions_row_->AddChildView(action_buttons_row_);

  // |inline_reply_| is a container for an inline textfield.
  inline_reply_ = new NotificationInputContainerMD(this);
  inline_reply_->SetVisible(false);
  actions_row_->AddChildView(inline_reply_);

  CreateOrUpdateViews(notification);
  UpdateControlButtonsVisibilityWithNotification(notification);

  set_notify_enter_exit_on_child(true);

  click_activator_ = std::make_unique<ClickActivator>(this);
  // Reasons to use pretarget handler instead of OnMousePressed:
  // - NotificationViewMD::OnMousePresssed would not fire on the inline reply
  //   textfield click in native notification.
  // - To make it look similar to ArcNotificationContentView::EventForwarder.
  AddPreTargetHandler(click_activator_.get());
}

NotificationViewMD::~NotificationViewMD() {
  RemovePreTargetHandler(click_activator_.get());
}

void NotificationViewMD::Layout() {
  MessageView::Layout();

  // We need to call IsExpandable() at the end of Layout() call, since whether
  // we should show expand button or not depends on the current view layout.
  // (e.g. Show expand button when |message_view_| exceeds one line.)
  header_row_->SetExpandButtonEnabled(IsExpandable());
  header_row_->Layout();

  // The notification background is rounded in MessageView::Layout(),
  // but we also have to round the actions row background here.
  if (actions_row_->visible()) {
    constexpr SkScalar kCornerRadius = SkIntToScalar(kNotificationCornerRadius);

    // Use vertically larger clip path, so that actions row's top coners will
    // not be rounded.
    gfx::Path path;
    gfx::Rect bounds = actions_row_->GetLocalBounds();
    bounds.set_y(bounds.y() - bounds.height());
    bounds.set_height(bounds.height() * 2);
    path.addRoundRect(gfx::RectToSkRect(bounds), kCornerRadius, kCornerRadius);

    action_buttons_row_->set_clip_path(path);
    inline_reply_->set_clip_path(path);
  }

  // The animation is needed to run inside of the border, which is shown only
  // when the notification is nested.
  if (is_nested()) {
    gfx::Rect ink_drop_bounds = GetLocalBounds();
    ink_drop_bounds.Inset(gfx::Insets(kNotificationBorderThickness));
    ink_drop_container_->SetBoundsRect(ink_drop_bounds);
  } else {
    ink_drop_container_->SetBoundsRect(GetLocalBounds());
  }
}

void NotificationViewMD::OnFocus() {
  MessageView::OnFocus();
  ScrollRectToVisible(GetLocalBounds());
}

void NotificationViewMD::ScrollRectToVisible(const gfx::Rect& rect) {
  // Notification want to show the whole notification when a part of it (like
  // a button) gets focused.
  views::View::ScrollRectToVisible(GetLocalBounds());
}

bool NotificationViewMD::OnMousePressed(const ui::MouseEvent& event) {
  last_mouse_pressed_timestamp_ = base::TimeTicks(event.time_stamp());
  return true;
}

bool NotificationViewMD::OnMouseDragged(const ui::MouseEvent& event) {
  return true;
}

void NotificationViewMD::OnMouseReleased(const ui::MouseEvent& event) {
  if (!event.IsOnlyLeftMouseButton())
    return;

  // The mouse has been clicked for a long time.
  if (ui::EventTimeStampToSeconds(event.time_stamp()) -
          ui::EventTimeStampToSeconds(last_mouse_pressed_timestamp_) >
      ui::GetGestureProviderConfig(
          ui::GestureProviderConfigType::CURRENT_PLATFORM)
          .gesture_detector_config.longpress_timeout.InSecondsF()) {
    ToggleInlineSettings(event);
    return;
  }

  // Ignore click of actions row outside action buttons.
  if (expanded_) {
    DCHECK(actions_row_);
    gfx::Point point_in_child = event.location();
    ConvertPointToTarget(this, actions_row_, &point_in_child);
    if (actions_row_->HitTestPoint(point_in_child))
      return;
  }

  // Ignore clicks of outside region when inline settings is shown.
  if (settings_row_ && settings_row_->visible())
    return;

  MessageView::OnMouseReleased(event);
}

void NotificationViewMD::OnMouseEvent(ui::MouseEvent* event) {
  switch (event->type()) {
    case ui::ET_MOUSE_ENTERED:
      UpdateControlButtonsVisibility();
      break;
    case ui::ET_MOUSE_EXITED:
      UpdateControlButtonsVisibility();
      break;
    default:
      break;
  }
  View::OnMouseEvent(event);
}

void NotificationViewMD::OnGestureEvent(ui::GestureEvent* event) {
  if (event->type() == ui::ET_GESTURE_LONG_TAP) {
    ToggleInlineSettings(*event);
    return;
  }
  MessageView::OnGestureEvent(event);
}

void NotificationViewMD::UpdateWithNotification(
    const Notification& notification) {
  MessageView::UpdateWithNotification(notification);
  UpdateControlButtonsVisibilityWithNotification(notification);

  CreateOrUpdateViews(notification);
  Layout();
  SchedulePaint();
}

// TODO(yoshiki): Move this to the parent class (MessageView).
void NotificationViewMD::UpdateControlButtonsVisibilityWithNotification(
    const Notification& notification) {
  control_buttons_view_->ShowSettingsButton(
      notification.should_show_settings_button());
  control_buttons_view_->ShowCloseButton(!GetPinned());
  UpdateControlButtonsVisibility();
}

void NotificationViewMD::ButtonPressed(views::Button* sender,
                                       const ui::Event& event) {
  // Certain operations can cause |this| to be destructed, so copy the members
  // we send to other parts of the code.
  // TODO(dewittj): Remove this hack.
  std::string id(notification_id());

  // Tapping anywhere on |header_row_| can expand the notification, though only
  // |expand_button| can be focused by TAB.
  if (sender == header_row_) {
    if (IsExpandable() && content_row_->visible()) {
      SetManuallyExpandedOrCollapsed(true);
      ToggleExpanded();
      Layout();
      SchedulePaint();
    }
    return;
  }

  // See if the button pressed was an action button.
  for (size_t i = 0; i < action_buttons_.size(); ++i) {
    if (sender != action_buttons_[i])
      continue;
    if (action_buttons_[i]->placeholder()) {
      inline_reply_->textfield()->set_index(i);
      inline_reply_->textfield()->set_placeholder(
          *action_buttons_[i]->placeholder());
      inline_reply_->textfield()->RequestFocus();
      inline_reply_->AnimateBackground(*event.AsLocatedEvent());
      inline_reply_->SetVisible(true);
      action_buttons_row_->SetVisible(false);
      Layout();
      SchedulePaint();
    } else {
      MessageCenter::Get()->ClickOnNotificationButton(id, i);
    }
    return;
  }

  if (sender == settings_done_button_) {
    if (block_all_button_->checked())
      MessageCenter::Get()->DisableNotification(id);
    ToggleInlineSettings(event);
    return;
  }
}

void NotificationViewMD::OnNotificationInputSubmit(size_t index,
                                                   const base::string16& text) {
  MessageCenter::Get()->ClickOnNotificationButtonWithReply(notification_id(),
                                                           index, text);
}

void NotificationViewMD::CreateOrUpdateContextTitleView(
    const Notification& notification) {
  header_row_->SetAccentColor(notification.accent_color() == SK_ColorTRANSPARENT
                                  ? kNotificationDefaultAccentColor
                                  : notification.accent_color());
  header_row_->SetTimestamp(notification.timestamp());

  base::string16 app_name = notification.display_source();
  if (notification.origin_url().is_valid() &&
      notification.origin_url().SchemeIsHTTPOrHTTPS()) {
    app_name = url_formatter::FormatUrlForSecurityDisplay(
        notification.origin_url(),
        url_formatter::SchemeDisplay::OMIT_HTTP_AND_HTTPS);
  } else if (app_name.empty() &&
             notification.notifier_id().type == NotifierId::SYSTEM_COMPONENT) {
    app_name = MessageCenter::Get()->GetSystemNotificationAppName();
  }
  header_row_->SetAppName(app_name);
}

void NotificationViewMD::CreateOrUpdateTitleView(
    const Notification& notification) {
  if (notification.title().empty() ||
      notification.type() == NOTIFICATION_TYPE_PROGRESS) {
    DCHECK(!title_view_ || left_content_->Contains(title_view_));
    delete title_view_;
    title_view_ = nullptr;
    return;
  }

  int title_character_limit =
      kNotificationWidth * kMaxTitleLines / kMinPixelsPerTitleCharacterMD;

  base::string16 title = gfx::TruncateString(
      notification.title(), title_character_limit, gfx::WORD_BREAK);
  if (!title_view_) {
    const gfx::FontList& font_list = GetTextFontList();

    title_view_ = new views::Label(title);
    title_view_->SetFontList(font_list);
    title_view_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title_view_->SetEnabledColor(kRegularTextColorMD);
    left_content_->AddChildView(title_view_);
  } else {
    title_view_->SetText(title);
  }
}

void NotificationViewMD::CreateOrUpdateMessageView(
    const Notification& notification) {
  if (notification.type() == NOTIFICATION_TYPE_PROGRESS ||
      notification.message().empty()) {
    // Deletion will also remove |message_view_| from its parent.
    delete message_view_;
    message_view_ = nullptr;
    return;
  }

  base::string16 text = gfx::TruncateString(
      notification.message(), kMessageCharacterLimitMD, gfx::WORD_BREAK);

  const gfx::FontList& font_list = GetTextFontList();

  if (!message_view_) {
    message_view_ = new BoundedLabel(text, font_list);
    message_view_->SetLineLimit(kMaxLinesForMessageView);
    message_view_->SetColor(kDimTextColorMD);

    left_content_->AddChildView(message_view_);
  } else {
    message_view_->SetText(text);
  }

  message_view_->SetVisible(notification.items().empty());
}

void NotificationViewMD::CreateOrUpdateCompactTitleMessageView(
    const Notification& notification) {
  if (notification.type() != NOTIFICATION_TYPE_PROGRESS) {
    DCHECK(!compact_title_message_view_ ||
           left_content_->Contains(compact_title_message_view_));
    delete compact_title_message_view_;
    compact_title_message_view_ = nullptr;
    return;
  }
  if (!compact_title_message_view_) {
    compact_title_message_view_ = new CompactTitleMessageView();
    left_content_->AddChildView(compact_title_message_view_);
  }

  compact_title_message_view_->set_title(notification.title());
  compact_title_message_view_->set_message(notification.message());
  left_content_->InvalidateLayout();
}

void NotificationViewMD::CreateOrUpdateProgressBarView(
    const Notification& notification) {
  if (notification.type() != NOTIFICATION_TYPE_PROGRESS) {
    DCHECK(!progress_bar_view_ || left_content_->Contains(progress_bar_view_));
    delete progress_bar_view_;
    progress_bar_view_ = nullptr;
    header_row_->ClearProgress();
    return;
  }

  DCHECK(left_content_);

  if (!progress_bar_view_) {
    progress_bar_view_ = new views::ProgressBar(kProgressBarHeight,
                                                /* allow_round_corner */ false);
    progress_bar_view_->SetBorder(
        views::CreateEmptyBorder(kProgressBarTopPadding, 0, 0, 0));
    left_content_->AddChildView(progress_bar_view_);
  }

  progress_bar_view_->SetValue(notification.progress() / 100.0);
  progress_bar_view_->SetVisible(notification.items().empty());

  if (0 <= notification.progress() && notification.progress() <= 100)
    header_row_->SetProgress(notification.progress());
  else
    header_row_->ClearProgress();
}

void NotificationViewMD::CreateOrUpdateProgressStatusView(
    const Notification& notification) {
  if (notification.type() != NOTIFICATION_TYPE_PROGRESS ||
      notification.progress_status().empty()) {
    if (!status_view_)
      return;
    DCHECK(left_content_->Contains(status_view_));
    delete status_view_;
    status_view_ = nullptr;
    return;
  }

  if (!status_view_) {
    const gfx::FontList& font_list = GetTextFontList();
    status_view_ = new views::Label();
    status_view_->SetFontList(font_list);
    status_view_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    status_view_->SetEnabledColor(kDimTextColorMD);
    status_view_->SetBorder(views::CreateEmptyBorder(kStatusTextPadding));
    left_content_->AddChildView(status_view_);
  }

  status_view_->SetText(notification.progress_status());
}

void NotificationViewMD::CreateOrUpdateListItemViews(
    const Notification& notification) {
  for (auto* item_view : item_views_)
    delete item_view;
  item_views_.clear();

  const std::vector<NotificationItem>& items = notification.items();

  for (size_t i = 0; i < items.size() && i < kMaxLinesForExpandedMessageView;
       ++i) {
    ItemView* item_view = new ItemView(items[i]);
    item_views_.push_back(item_view);
    left_content_->AddChildView(item_view);
  }

  list_items_count_ = items.size();

  // Needed when CreateOrUpdateViews is called for update.
  if (!item_views_.empty())
    left_content_->InvalidateLayout();
}

void NotificationViewMD::CreateOrUpdateIconView(
    const Notification& notification) {
  const bool use_image_for_icon = notification.icon().IsEmpty();

  gfx::ImageSkia icon = use_image_for_icon ? notification.image().AsImageSkia()
                                           : notification.icon().AsImageSkia();

  if (notification.type() == NOTIFICATION_TYPE_PROGRESS ||
      notification.type() == NOTIFICATION_TYPE_MULTIPLE || icon.isNull()) {
    DCHECK(!icon_view_ || right_content_->Contains(icon_view_));
    delete icon_view_;
    icon_view_ = nullptr;
    return;
  }

  if (!icon_view_) {
    icon_view_ = new ProportionalImageView(kIconViewSize);
    right_content_->AddChildView(icon_view_);
  }

  icon_view_->SetImage(icon, icon.size());

  // Hide the icon on the right side when the notification is expanded.
  hide_icon_on_expanded_ = use_image_for_icon;
}

void NotificationViewMD::CreateOrUpdateSmallIconView(
    const Notification& notification) {
  if (!notification.vector_small_image().is_empty()) {
    header_row_->SetAppIcon(
        gfx::CreateVectorIcon(notification.vector_small_image(),
                              kSmallImageSizeMD, notification.accent_color()));
  } else if (!notification.small_image().IsEmpty()) {
    header_row_->SetAppIcon(notification.small_image().AsImageSkia());
  } else {
    header_row_->ClearAppIcon();
  }
}

void NotificationViewMD::CreateOrUpdateImageView(
    const Notification& notification) {
  if (notification.image().IsEmpty()) {
    if (image_container_view_) {
      DCHECK(Contains(image_container_view_));
      delete image_container_view_;
      image_container_view_ = nullptr;
    }
    return;
  }

  if (!image_container_view_) {
    image_container_view_ = new LargeImageContainerView();
    // Insert the created image container just after the |content_row_|.
    AddChildViewAt(image_container_view_, GetIndexOf(content_row_) + 1);
  }

  image_container_view_->SetImage(notification.image().AsImageSkia());
}

void NotificationViewMD::CreateOrUpdateActionButtonViews(
    const Notification& notification) {
  const std::vector<ButtonInfo>& buttons = notification.buttons();
  bool new_buttons = action_buttons_.size() != buttons.size();

  if (new_buttons || buttons.size() == 0) {
    for (auto* item : action_buttons_)
      delete item;
    action_buttons_.clear();
  }

  DCHECK_EQ(this, actions_row_->parent());

  for (size_t i = 0; i < buttons.size(); ++i) {
    ButtonInfo button_info = buttons[i];
    if (new_buttons) {
      NotificationButtonMD* button = new NotificationButtonMD(
          this, button_info.title, button_info.placeholder);
      action_buttons_.push_back(button);
      action_buttons_row_->AddChildView(button);
    } else {
      action_buttons_[i]->SetText(button_info.title);
      action_buttons_[i]->SchedulePaint();
      action_buttons_[i]->Layout();
    }

    // Change action button color to the accent color.
    action_buttons_[i]->SetEnabledTextColors(notification.accent_color() ==
                                                     SK_ColorTRANSPARENT
                                                 ? kActionButtonTextColor
                                                 : notification.accent_color());
  }

  // Inherit mouse hover state when action button views reset.
  // If the view is not expanded, there should be no hover state.
  if (new_buttons && expanded_) {
    views::Widget* widget = GetWidget();
    if (widget) {
      // This Layout() is needed because button should be in the right location
      // in the view hierarchy when SynthesizeMouseMoveEvent() is called.
      Layout();
      widget->SetSize(widget->GetContentsView()->GetPreferredSize());
      GetWidget()->SynthesizeMouseMoveEvent();
    }
  }
}

void NotificationViewMD::CreateOrUpdateInlineSettingsViews(
    const Notification& notification) {
  if (settings_row_) {
    DCHECK_EQ(SettingsButtonHandler::INLINE,
              notification.rich_notification_data().settings_button_handler);
    return;
  }

  if (notification.rich_notification_data().settings_button_handler !=
      SettingsButtonHandler::INLINE) {
    return;
  }

  // |settings_row_| contains inline settings.
  settings_row_ = new views::View();
  settings_row_->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kVertical, kSettingsRowPadding, 0));

  int block_notifications_message_id = 0;
  switch (notification.notifier_id().type) {
    case NotifierId::APPLICATION:
    case NotifierId::ARC_APPLICATION:
      block_notifications_message_id =
          IDS_MESSAGE_CENTER_BLOCK_ALL_NOTIFICATIONS_APP;
      break;
    case NotifierId::WEB_PAGE:
      block_notifications_message_id =
          IDS_MESSAGE_CENTER_BLOCK_ALL_NOTIFICATIONS_SITE;
      break;
    case NotifierId::SYSTEM_COMPONENT:
    case NotifierId::SIZE:
      block_notifications_message_id =
          IDS_MESSAGE_CENTER_BLOCK_ALL_NOTIFICATIONS;
      break;
  }
  DCHECK_NE(block_notifications_message_id, 0);

  block_all_button_ = new InlineSettingsRadioButton(
      l10n_util::GetStringUTF16(block_notifications_message_id));
  block_all_button_->set_listener(this);
  block_all_button_->SetBorder(
      views::CreateEmptyBorder(kSettingsRadioButtonPadding));
  settings_row_->AddChildView(block_all_button_);

  dont_block_button_ = new InlineSettingsRadioButton(
      l10n_util::GetStringUTF16(IDS_MESSAGE_CENTER_DONT_BLOCK_NOTIFICATIONS));
  dont_block_button_->set_listener(this);
  dont_block_button_->SetBorder(
      views::CreateEmptyBorder(kSettingsRadioButtonPadding));
  settings_row_->AddChildView(dont_block_button_);
  settings_row_->SetVisible(false);

  settings_done_button_ = new NotificationButtonMD(
      this, l10n_util::GetStringUTF16(IDS_MESSAGE_CENTER_SETTINGS_DONE),
      base::nullopt);
  settings_done_button_->SetTextSubpixelRenderingEnabled(false);

  auto* settings_button_row = new views::View;
  auto settings_button_layout = std::make_unique<views::BoxLayout>(
      views::BoxLayout::kHorizontal, kSettingsButtonRowPadding, 0);
  settings_button_layout->set_main_axis_alignment(
      views::BoxLayout::MAIN_AXIS_ALIGNMENT_END);
  settings_button_row->SetLayoutManager(std::move(settings_button_layout));
  settings_button_row->AddChildView(settings_done_button_);
  settings_row_->AddChildView(settings_button_row);

  AddChildViewAt(settings_row_, GetIndexOf(actions_row_));
}

bool NotificationViewMD::IsExpandable() {
  // Expandable if the message exceeds one line.
  if (message_view_ && message_view_->visible() &&
      message_view_->GetLinesForWidthAndLimit(message_view_->width(), -1) > 1) {
    return true;
  }
  // Expandable if there is at least one inline action.
  if (action_buttons_row_->has_children())
    return true;

  // Expandable if the notification has image.
  if (image_container_view_)
    return true;

  // Expandable if there are multiple list items.
  if (item_views_.size() > 1)
    return true;

  // TODO(fukino): Expandable if both progress bar and message exist.

  return false;
}

void NotificationViewMD::ToggleExpanded() {
  SetExpanded(!expanded_);
}

void NotificationViewMD::UpdateViewForExpandedState(bool expanded) {
  header_row_->SetExpanded(expanded);
  if (message_view_) {
    message_view_->SetLineLimit(expanded ? kMaxLinesForExpandedMessageView
                                         : kMaxLinesForMessageView);
  }
  if (image_container_view_)
    image_container_view_->SetVisible(expanded);

  actions_row_->SetVisible(expanded && (action_buttons_row_->has_children()));
  if (!expanded) {
    action_buttons_row_->SetVisible(true);
    inline_reply_->SetVisible(false);
  }

  for (size_t i = kMaxLinesForMessageView; i < item_views_.size(); ++i) {
    item_views_[i]->SetVisible(expanded);
  }
  if (status_view_)
    status_view_->SetVisible(expanded);
  header_row_->SetOverflowIndicator(
      list_items_count_ -
      (expanded ? item_views_.size() : kMaxLinesForMessageView));

  right_content_->SetVisible(icon_view_ &&
                             (!hide_icon_on_expanded_ || !expanded));
  if (right_content_->visible()) {
    left_content_->SetBorder(
        views::CreateEmptyBorder(kLeftContentPaddingWithIcon));

    // TODO(tetsui): Workaround https://crbug.com/682266 by explicitly setting
    // the width.
    // Ideally, we should fix the original bug, but it seems there's no obvious
    // solution for the bug according to https://crbug.com/678337#c7, we should
    // ensure that the change won't break any of the users of BoxLayout class.
    if (message_view_)
      message_view_->SizeToFit(kMessageViewWidthWithIcon);
  } else {
    left_content_->SetBorder(views::CreateEmptyBorder(kLeftContentPadding));
    if (message_view_)
      message_view_->SizeToFit(kMessageViewWidth);
  }
}

void NotificationViewMD::ToggleInlineSettings(const ui::Event& event) {
  if (!settings_row_)
    return;

  bool inline_settings_visible = !settings_row_->visible();

  settings_row_->SetVisible(inline_settings_visible);
  content_row_->SetVisible(!inline_settings_visible);

  // Always check "Don't block" when inline settings is shown.
  // If it's already blocked, users should not see inline settings.
  // Toggling should reset the state.
  dont_block_button_->SetChecked(true);

  SetExpanded(!inline_settings_visible);

  PreferredSizeChanged();

  if (inline_settings_visible)
    AddBackgroundAnimation(event);
  else
    RemoveBackgroundAnimation();

  Layout();
  SchedulePaint();
}

// TODO(yoshiki): Move this to the parent class (MessageView) and share the code
// among NotificationView and ArcNotificationView.
void NotificationViewMD::UpdateControlButtonsVisibility() {
  const bool target_visibility =
      (AlwaysShowControlButtons() || IsMouseHovered() ||
       control_buttons_view_->IsCloseButtonFocused() ||
       control_buttons_view_->IsSettingsButtonFocused()) &&
      !GetPinned();

  control_buttons_view_->SetVisible(target_visibility);
}

NotificationControlButtonsView* NotificationViewMD::GetControlButtonsView()
    const {
  return control_buttons_view_.get();
}

bool NotificationViewMD::IsExpanded() const {
  return expanded_;
}

void NotificationViewMD::SetExpanded(bool expanded) {
  if (expanded_ == expanded)
    return;
  expanded_ = expanded;

  UpdateViewForExpandedState(expanded_);
  content_row_->InvalidateLayout();
  PreferredSizeChanged();
}

bool NotificationViewMD::IsManuallyExpandedOrCollapsed() const {
  return manually_expanded_or_collapsed_;
}

void NotificationViewMD::SetManuallyExpandedOrCollapsed(bool value) {
  manually_expanded_or_collapsed_ = value;
}

void NotificationViewMD::OnSettingsButtonPressed(const ui::Event& event) {
  if (settings_row_)
    ToggleInlineSettings(event);
  else
    MessageView::OnSettingsButtonPressed(event);
}

void NotificationViewMD::Activate() {
  GetWidget()->widget_delegate()->set_can_activate(true);
  GetWidget()->Activate();
}

void NotificationViewMD::AddBackgroundAnimation(const ui::Event& event) {
  SetInkDropMode(InkDropMode::ON_NO_GESTURE_HANDLER);
  // In case the animation is triggered from keyboard operation.
  if (!event.IsLocatedEvent()) {
    AnimateInkDrop(views::InkDropState::ACTION_PENDING, nullptr);
    return;
  }

  // Convert the point of |event| from the coordinate system of
  // |control_buttons_view_| to that of NotificationViewMD, create a new
  // LocatedEvent which has the new point.
  views::View* target = static_cast<views::View*>(event.target());
  const gfx::Point& location = event.AsLocatedEvent()->location();
  gfx::Point converted_location(location);
  View::ConvertPointToTarget(target, this, &converted_location);
  std::unique_ptr<ui::Event> cloned_event = ui::Event::Clone(event);
  ui::LocatedEvent* cloned_located_event = cloned_event->AsLocatedEvent();
  cloned_located_event->set_location(converted_location);

  if (View::HitTestPoint(event.AsLocatedEvent()->location())) {
    AnimateInkDrop(views::InkDropState::ACTION_PENDING,
                   ui::LocatedEvent::FromIfValid(cloned_located_event));
  }
}

void NotificationViewMD::RemoveBackgroundAnimation() {
  AnimateInkDrop(views::InkDropState::HIDDEN, nullptr);
}

void NotificationViewMD::AddInkDropLayer(ui::Layer* ink_drop_layer) {
  GetInkDrop()->AddObserver(this);
  header_row_->SetPaintToLayer();
  header_row_->layer()->SetFillsBoundsOpaquely(false);
  block_all_button_->SetPaintToLayer();
  block_all_button_->layer()->SetFillsBoundsOpaquely(false);
  dont_block_button_->SetPaintToLayer();
  dont_block_button_->layer()->SetFillsBoundsOpaquely(false);
  settings_done_button_->SetPaintToLayer();
  settings_done_button_->layer()->SetFillsBoundsOpaquely(false);
  ink_drop_container_->AddInkDropLayer(ink_drop_layer);
  InstallInkDropMask(ink_drop_layer);
}

void NotificationViewMD::RemoveInkDropLayer(ui::Layer* ink_drop_layer) {
  header_row_->DestroyLayer();
  block_all_button_->DestroyLayer();
  dont_block_button_->DestroyLayer();
  settings_done_button_->DestroyLayer();
  ResetInkDropMask();
  ink_drop_container_->RemoveInkDropLayer(ink_drop_layer);
  GetInkDrop()->RemoveObserver(this);
}

std::unique_ptr<views::InkDropRipple> NotificationViewMD::CreateInkDropRipple()
    const {
  return std::make_unique<views::FloodFillInkDropRipple>(
      ink_drop_container_->size(), GetInkDropCenterBasedOnLastEvent(),
      GetInkDropBaseColor(), ink_drop_visible_opacity());
}

SkColor NotificationViewMD::GetInkDropBaseColor() const {
  return kSettingsRowBackgroundColor;
}

void NotificationViewMD::InkDropAnimationStarted() {
  header_row_->SetSubpixelRenderingEnabled(false);
}

void NotificationViewMD::InkDropRippleAnimationEnded(
    views::InkDropState ink_drop_state) {
  if (ink_drop_state == views::InkDropState::HIDDEN)
    header_row_->SetSubpixelRenderingEnabled(true);
}

}  // namespace message_center
