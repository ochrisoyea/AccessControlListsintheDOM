// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/payments/payment_request_views_util.h"

#include <vector>

#include "base/macros.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/views/harmony/chrome_typography.h"
#include "chrome/browser/ui/views/payments/payment_request_dialog_view_ids.h"
#include "chrome/browser/ui/views/payments/payment_request_sheet_controller.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/grit/theme_resources.h"
#include "components/autofill/core/browser/autofill_data_util.h"
#include "components/autofill/core/browser/autofill_profile.h"
#include "components/autofill/core/browser/autofill_type.h"
#include "components/autofill/core/browser/credit_card.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/browser/phone_number_i18n.h"
#include "components/payments/core/payment_options_provider.h"
#include "components/payments/core/payment_request_data_util.h"
#include "components/payments/core/payments_profile_comparator.h"
#include "components/payments/core/strings_util.h"
#include "components/strings/grit/components_strings.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/default_style.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/styled_label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/grid_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/painter.h"
#include "ui/views/view.h"

namespace payments {

namespace {

// |s1|, |s2|, and |s3| are lines identifying the profile. |s1| is the
// "headline" which may be emphasized depending on |type|. If |enabled| is
// false, the labels will look disabled.
std::unique_ptr<views::View> GetBaseProfileLabel(
    AddressStyleType type,
    const base::string16& s1,
    const base::string16& s2,
    const base::string16& s3,
    base::string16* accessible_content,
    bool enabled = true) {
  DCHECK(accessible_content);
  std::unique_ptr<views::View> container = std::make_unique<views::View>();
  std::unique_ptr<views::BoxLayout> layout = std::make_unique<views::BoxLayout>(
      views::BoxLayout::kVertical, gfx::Insets(), 0);
  layout->set_cross_axis_alignment(
      views::BoxLayout::CROSS_AXIS_ALIGNMENT_START);
  container->SetLayoutManager(std::move(layout));

  if (!s1.empty()) {
    const int text_style = type == AddressStyleType::DETAILED
                               ? static_cast<int>(STYLE_EMPHASIZED)
                               : static_cast<int>(views::style::STYLE_PRIMARY);
    std::unique_ptr<views::Label> label = std::make_unique<views::Label>(
        s1, views::style::CONTEXT_LABEL, text_style);
    label->set_id(static_cast<int>(DialogViewID::PROFILE_LABEL_LINE_1));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    if (!enabled) {
      label->SetEnabledColor(label->GetNativeTheme()->GetSystemColor(
          ui::NativeTheme::kColorId_LabelDisabledColor));
    }
    container->AddChildView(label.release());
  }

  if (!s2.empty()) {
    std::unique_ptr<views::Label> label = std::make_unique<views::Label>(s2);
    label->set_id(static_cast<int>(DialogViewID::PROFILE_LABEL_LINE_2));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    if (!enabled) {
      label->SetEnabledColor(label->GetNativeTheme()->GetSystemColor(
          ui::NativeTheme::kColorId_LabelDisabledColor));
    }
    container->AddChildView(label.release());
  }

  if (!s3.empty()) {
    std::unique_ptr<views::Label> label = std::make_unique<views::Label>(s3);
    label->set_id(static_cast<int>(DialogViewID::PROFILE_LABEL_LINE_3));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    if (!enabled) {
      label->SetEnabledColor(label->GetNativeTheme()->GetSystemColor(
          ui::NativeTheme::kColorId_LabelDisabledColor));
    }
    container->AddChildView(label.release());
  }

  *accessible_content = l10n_util::GetStringFUTF16(
      IDS_PAYMENTS_PROFILE_LABELS_ACCESSIBLE_FORMAT, s1, s2, s3);

  return container;
}

// Returns a label representing the |profile| as a shipping address. See
// GetBaseProfileLabel() for more documentation.
std::unique_ptr<views::View> GetShippingAddressLabel(
    AddressStyleType type,
    const std::string& locale,
    const autofill::AutofillProfile& profile,
    base::string16* accessible_content,
    bool enabled) {
  DCHECK(accessible_content);
  base::string16 name = profile.GetInfo(autofill::NAME_FULL, locale);

  base::string16 address =
      GetShippingAddressLabelFormAutofillProfile(profile, locale);

  base::string16 phone =
      autofill::i18n::GetFormattedPhoneNumberForDisplay(profile, locale);

  return GetBaseProfileLabel(type, name, address, phone, accessible_content,
                             enabled);
}

std::unique_ptr<views::Label> GetLabelForMissingInformation(
    const base::string16& missing_info) {
  std::unique_ptr<views::Label> label =
      std::make_unique<views::Label>(missing_info, CONTEXT_DEPRECATED_SMALL);
  label->set_id(static_cast<int>(DialogViewID::PROFILE_LABEL_ERROR));
  // Missing information typically has a nice shade of blue.
  label->SetEnabledColor(label->GetNativeTheme()->GetSystemColor(
      ui::NativeTheme::kColorId_LinkEnabled));
  return label;
}

// Paints the gray horizontal line that doesn't span the entire width of the
// dialog at the bottom of the view it borders.
class PaymentRequestRowBorderPainter : public views::Painter {
 public:
  explicit PaymentRequestRowBorderPainter(SkColor color) : color_(color) {}
  ~PaymentRequestRowBorderPainter() override {}

  // views::Painter:
  gfx::Size GetMinimumSize() const override {
    return gfx::Size(2 * payments::kPaymentRequestRowHorizontalInsets, 1);
  }

  void Paint(gfx::Canvas* canvas, const gfx::Size& size) override {
    int line_height = size.height() - 1;
    canvas->DrawLine(
        gfx::PointF(payments::kPaymentRequestRowHorizontalInsets, line_height),
        gfx::PointF(size.width() - payments::kPaymentRequestRowHorizontalInsets,
                    line_height),
        color_);
  }

 private:
  SkColor color_;
  DISALLOW_COPY_AND_ASSIGN(PaymentRequestRowBorderPainter);
};

}  // namespace

int GetActualDialogWidth() {
  static int actual_width =
      views::LayoutProvider::Get()->GetSnappedDialogWidth(kDialogMinWidth);
  return actual_width;
}

void PopulateSheetHeaderView(bool show_back_arrow,
                             std::unique_ptr<views::View> header_content_view,
                             views::ButtonListener* listener,
                             views::View* container,
                             std::unique_ptr<views::Background> background) {
  SkColor background_color = background->get_color();
  container->SetBackground(std::move(background));
  views::GridLayout* layout = container->SetLayoutManager(
      std::make_unique<views::GridLayout>(container));

  constexpr int kVerticalInset = 14;
  constexpr int kHeaderHorizontalInset = 16;
  container->SetBorder(
      views::CreateEmptyBorder(kVerticalInset, kHeaderHorizontalInset,
                               kVerticalInset, kHeaderHorizontalInset));

  views::ColumnSet* columns = layout->AddColumnSet(0);
  // A column for the optional back arrow.
  columns->AddColumn(views::GridLayout::LEADING, views::GridLayout::CENTER, 0,
                     views::GridLayout::USE_PREF, 0, 0);

  constexpr int kPaddingBetweenArrowAndTitle = 8;
  if (show_back_arrow)
    columns->AddPaddingColumn(0, kPaddingBetweenArrowAndTitle);

  // A column for the title.
  columns->AddColumn(views::GridLayout::FILL, views::GridLayout::CENTER, 1,
                     views::GridLayout::USE_PREF, 0, 0);

  layout->StartRow(0, 0);
  if (!show_back_arrow) {
    layout->SkipColumns(1);
  } else {
    views::ImageButton* back_arrow = views::CreateVectorImageButton(listener);
    views::SetImageFromVectorIcon(
        back_arrow, vector_icons::kBackArrowIcon,
        GetForegroundColorForBackground(background_color));
    constexpr int kBackArrowSize = 16;
    back_arrow->SetSize(gfx::Size(kBackArrowSize, kBackArrowSize));
    back_arrow->SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
    back_arrow->set_tag(
        static_cast<int>(PaymentRequestCommonTags::BACK_BUTTON_TAG));
    back_arrow->set_id(static_cast<int>(DialogViewID::BACK_BUTTON));
    back_arrow->SetAccessibleName(l10n_util::GetStringUTF16(IDS_PAYMENTS_BACK));
    layout->AddView(back_arrow);
  }

  layout->AddView(header_content_view.release());
}

std::unique_ptr<views::ImageView> CreateInstrumentIconView(
    int icon_resource_id,
    const gfx::ImageSkia* img,
    const base::string16& tooltip_text,
    float opacity) {
  std::unique_ptr<views::ImageView> icon_view =
      std::make_unique<views::ImageView>();
  icon_view->set_can_process_events_within_subtree(false);
  if (img != nullptr) {
    icon_view->SetImage(*img);
  } else {
    icon_view->SetImage(ui::ResourceBundle::GetSharedInstance()
                            .GetImageNamed(icon_resource_id)
                            .AsImageSkia());
  }
  icon_view->SetTooltipText(tooltip_text);
  icon_view->SetPaintToLayer();
  icon_view->layer()->SetFillsBoundsOpaquely(false);
  icon_view->layer()->SetOpacity(opacity);
  return icon_view;
}

std::unique_ptr<views::View> CreateProductLogoFooterView() {
  std::unique_ptr<views::View> content_view = std::make_unique<views::View>();

  auto layout = std::make_unique<views::BoxLayout>(
      views::BoxLayout::kHorizontal, gfx::Insets(), 0);
  layout->set_main_axis_alignment(views::BoxLayout::MAIN_AXIS_ALIGNMENT_START);
  layout->set_cross_axis_alignment(
      views::BoxLayout::CROSS_AXIS_ALIGNMENT_START);
  content_view->SetLayoutManager(std::move(layout));

  // Adds the Chrome logo image.
  std::unique_ptr<views::ImageView> chrome_logo =
      std::make_unique<views::ImageView>();
  chrome_logo->set_can_process_events_within_subtree(false);
  chrome_logo->SetImage(ui::ResourceBundle::GetSharedInstance()
                            .GetImageNamed(IDR_PRODUCT_LOGO_NAME_22)
                            .AsImageSkia());
  chrome_logo->SetTooltipText(l10n_util::GetStringUTF16(IDS_PRODUCT_NAME));
  content_view->AddChildView(chrome_logo.release());

  return content_view;
}

std::unique_ptr<views::View> GetShippingAddressLabelWithMissingInfo(
    AddressStyleType type,
    const std::string& locale,
    const autofill::AutofillProfile& profile,
    const PaymentsProfileComparator& comp,
    base::string16* accessible_content,
    bool enabled) {
  DCHECK(accessible_content);
  std::unique_ptr<views::View> base_label = GetShippingAddressLabel(
      type, locale, profile, accessible_content, enabled);

  base::string16 missing = comp.GetStringForMissingShippingFields(profile);
  if (!missing.empty()) {
    base_label->AddChildView(GetLabelForMissingInformation(missing).release());
    *accessible_content = l10n_util::GetStringFUTF16(
        IDS_PAYMENTS_ACCESSIBLE_LABEL_WITH_ERROR, *accessible_content, missing);
  }

  return base_label;
}

// TODO(anthonyvd): unit test the label layout.
std::unique_ptr<views::View> GetContactInfoLabel(
    AddressStyleType type,
    const std::string& locale,
    const autofill::AutofillProfile& profile,
    const PaymentOptionsProvider& options,
    const PaymentsProfileComparator& comp,
    base::string16* accessible_content) {
  DCHECK(accessible_content);
  base::string16 name = options.request_payer_name()
                            ? profile.GetInfo(autofill::NAME_FULL, locale)
                            : base::string16();

  base::string16 phone =
      options.request_payer_phone()
          ? autofill::i18n::GetFormattedPhoneNumberForDisplay(profile, locale)
          : base::string16();

  base::string16 email = options.request_payer_email()
                             ? profile.GetInfo(autofill::EMAIL_ADDRESS, locale)
                             : base::string16();

  std::unique_ptr<views::View> base_label =
      GetBaseProfileLabel(type, name, phone, email, accessible_content);

  base::string16 missing = comp.GetStringForMissingContactFields(profile);
  if (!missing.empty()) {
    base_label->AddChildView(GetLabelForMissingInformation(missing).release());
    *accessible_content = l10n_util::GetStringFUTF16(
        IDS_PAYMENTS_ACCESSIBLE_LABEL_WITH_ERROR, *accessible_content, missing);
  }
  return base_label;
}

std::unique_ptr<views::Border> CreatePaymentRequestRowBorder(
    SkColor color,
    const gfx::Insets& insets) {
  return views::CreateBorderPainter(
      std::make_unique<PaymentRequestRowBorderPainter>(color), insets);
}

std::unique_ptr<views::Label> CreateBoldLabel(const base::string16& text) {
  return std::make_unique<views::Label>(text, views::style::CONTEXT_LABEL,
                                        STYLE_EMPHASIZED);
}

std::unique_ptr<views::Label> CreateMediumLabel(const base::string16& text) {
  // TODO(tapted): This should refer to a style in the Chrome typography spec.
  // Also, it needs to handle user setups where the default font is BOLD already
  // since asking for a MEDIUM font will give a lighter font.
  std::unique_ptr<views::Label> label = std::make_unique<views::Label>(text);
  label->SetFontList(
      ui::ResourceBundle::GetSharedInstance().GetFontListWithDelta(
          ui::kLabelFontSizeDelta, gfx::Font::NORMAL,
          gfx::Font::Weight::MEDIUM));
  return label;
}

std::unique_ptr<views::Label> CreateHintLabel(
    const base::string16& text,
    gfx::HorizontalAlignment alignment) {
  std::unique_ptr<views::Label> label = std::make_unique<views::Label>(
      text, views::style::CONTEXT_LABEL, STYLE_HINT);
  label->SetHorizontalAlignment(alignment);
  return label;
}

std::unique_ptr<views::View> CreateShippingOptionLabel(
    payments::mojom::PaymentShippingOption* shipping_option,
    const base::string16& formatted_amount,
    bool emphasize_label,
    base::string16* accessible_content) {
  DCHECK(accessible_content);
  std::unique_ptr<views::View> container = std::make_unique<views::View>();

  auto layout = std::make_unique<views::BoxLayout>(views::BoxLayout::kVertical,
                                                   gfx::Insets(), 0);
  layout->set_cross_axis_alignment(
      views::BoxLayout::CROSS_AXIS_ALIGNMENT_START);
  container->SetLayoutManager(std::move(layout));

  if (shipping_option) {
    const base::string16& text = base::UTF8ToUTF16(shipping_option->label);
    std::unique_ptr<views::Label> shipping_label =
        emphasize_label ? CreateMediumLabel(text)
                        : std::make_unique<views::Label>(text);
    // Strings from the website may not match the locale of the device, so align
    // them according to the language of the text. This will result, for
    // example, in "he" labels being right-aligned in a browser that's using
    // "en" locale.
    shipping_label->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
    shipping_label->set_id(
        static_cast<int>(DialogViewID::SHIPPING_OPTION_DESCRIPTION));
    container->AddChildView(shipping_label.release());

    std::unique_ptr<views::Label> amount_label =
        std::make_unique<views::Label>(formatted_amount);
    amount_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    amount_label->set_id(
        static_cast<int>(DialogViewID::SHIPPING_OPTION_AMOUNT));
    container->AddChildView(amount_label.release());

    *accessible_content = l10n_util::GetStringFUTF16(
        IDS_PAYMENTS_PROFILE_LABELS_ACCESSIBLE_FORMAT, text, formatted_amount,
        base::string16());
  }

  return container;
}

SkColor GetForegroundColorForBackground(SkColor background_color) {
  constexpr double kLightForegroundRatioThreshold = 3;
  if (background_color != 0 &&
      color_utils::GetContrastRatio(background_color, SK_ColorWHITE) >=
          kLightForegroundRatioThreshold) {
    return SK_ColorWHITE;
  }
  views::Label label;
  return label.enabled_color();
}

}  // namespace payments
