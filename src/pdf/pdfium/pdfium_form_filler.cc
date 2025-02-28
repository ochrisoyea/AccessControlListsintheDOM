// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "pdf/pdfium/pdfium_form_filler.h"

#include <algorithm>
#include <string>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "pdf/pdfium/pdfium_engine.h"

namespace chrome_pdf {

namespace {

std::string WideStringToString(FPDF_WIDESTRING wide_string) {
  return base::UTF16ToUTF8(reinterpret_cast<const base::char16*>(wide_string));
}

}  // namespace

PDFiumFormFiller::PDFiumFormFiller(PDFiumEngine* engine) : engine_(engine) {
  // Initialize FPDF_FORMFILLINFO member variables.  Deriving from this struct
  // allows the static callbacks to be able to cast the FPDF_FORMFILLINFO in
  // callbacks to ourself instead of maintaining a map of them to
  // PDFiumEngine.
  FPDF_FORMFILLINFO::version = 1;
  FPDF_FORMFILLINFO::m_pJsPlatform = this;
  FPDF_FORMFILLINFO::Release = nullptr;
  FPDF_FORMFILLINFO::FFI_Invalidate = Form_Invalidate;
  FPDF_FORMFILLINFO::FFI_OutputSelectedRect = Form_OutputSelectedRect;
  FPDF_FORMFILLINFO::FFI_SetCursor = Form_SetCursor;
  FPDF_FORMFILLINFO::FFI_SetTimer = Form_SetTimer;
  FPDF_FORMFILLINFO::FFI_KillTimer = Form_KillTimer;
  FPDF_FORMFILLINFO::FFI_GetLocalTime = Form_GetLocalTime;
  FPDF_FORMFILLINFO::FFI_OnChange = Form_OnChange;
  FPDF_FORMFILLINFO::FFI_GetPage = Form_GetPage;
  FPDF_FORMFILLINFO::FFI_GetCurrentPage = Form_GetCurrentPage;
  FPDF_FORMFILLINFO::FFI_GetRotation = Form_GetRotation;
  FPDF_FORMFILLINFO::FFI_ExecuteNamedAction = Form_ExecuteNamedAction;
  FPDF_FORMFILLINFO::FFI_SetTextFieldFocus = Form_SetTextFieldFocus;
  FPDF_FORMFILLINFO::FFI_DoURIAction = Form_DoURIAction;
  FPDF_FORMFILLINFO::FFI_DoGoToAction = Form_DoGoToAction;
#if defined(PDF_ENABLE_XFA)
  FPDF_FORMFILLINFO::version = 2;
  FPDF_FORMFILLINFO::FFI_EmailTo = Form_EmailTo;
  FPDF_FORMFILLINFO::FFI_DisplayCaret = Form_DisplayCaret;
  FPDF_FORMFILLINFO::FFI_SetCurrentPage = Form_SetCurrentPage;
  FPDF_FORMFILLINFO::FFI_GetCurrentPageIndex = Form_GetCurrentPageIndex;
  FPDF_FORMFILLINFO::FFI_GetPageViewRect = Form_GetPageViewRect;
  FPDF_FORMFILLINFO::FFI_GetPlatform = Form_GetPlatform;
  FPDF_FORMFILLINFO::FFI_PageEvent = nullptr;
  FPDF_FORMFILLINFO::FFI_PopupMenu = Form_PopupMenu;
  FPDF_FORMFILLINFO::FFI_PostRequestURL = Form_PostRequestURL;
  FPDF_FORMFILLINFO::FFI_PutRequestURL = Form_PutRequestURL;
  FPDF_FORMFILLINFO::FFI_UploadTo = Form_UploadTo;
  FPDF_FORMFILLINFO::FFI_DownloadFromURL = Form_DownloadFromURL;
  FPDF_FORMFILLINFO::FFI_OpenFile = Form_OpenFile;
  FPDF_FORMFILLINFO::FFI_GotoURL = Form_GotoURL;
  FPDF_FORMFILLINFO::FFI_GetLanguage = Form_GetLanguage;
#endif  // defined(PDF_ENABLE_XFA)

  IPDF_JSPLATFORM::version = 3;
  IPDF_JSPLATFORM::app_alert = Form_Alert;
  IPDF_JSPLATFORM::app_beep = Form_Beep;
  IPDF_JSPLATFORM::app_response = Form_Response;
  IPDF_JSPLATFORM::Doc_getFilePath = Form_GetFilePath;
  IPDF_JSPLATFORM::Doc_mail = Form_Mail;
  IPDF_JSPLATFORM::Doc_print = Form_Print;
  IPDF_JSPLATFORM::Doc_submitForm = Form_SubmitForm;
  IPDF_JSPLATFORM::Doc_gotoPage = Form_GotoPage;
  IPDF_JSPLATFORM::Field_browse = nullptr;
}

// static
void PDFiumFormFiller::Form_Invalidate(FPDF_FORMFILLINFO* param,
                                       FPDF_PAGE page,
                                       double left,
                                       double top,
                                       double right,
                                       double bottom) {
  PDFiumEngine* engine = GetEngine(param);
  int page_index = engine->GetVisiblePageIndex(page);
  if (page_index == -1) {
    // This can sometime happen when the page is closed because it went off
    // screen, and PDFium invalidates the control as it's being deleted.
    return;
  }

  pp::Rect rect = engine->pages_[page_index]->PageToScreen(
      engine->GetVisibleRect().point(), engine->current_zoom_, left, top, right,
      bottom, engine->current_rotation_);
  engine->client_->Invalidate(rect);
}

// static
void PDFiumFormFiller::Form_OutputSelectedRect(FPDF_FORMFILLINFO* param,
                                               FPDF_PAGE page,
                                               double left,
                                               double top,
                                               double right,
                                               double bottom) {
  PDFiumEngine* engine = GetEngine(param);
  int page_index = engine->GetVisiblePageIndex(page);
  if (page_index == -1) {
    NOTREACHED();
    return;
  }
  pp::Rect rect = engine->pages_[page_index]->PageToScreen(
      engine->GetVisibleRect().point(), engine->current_zoom_, left, top, right,
      bottom, engine->current_rotation_);
  if (rect.IsEmpty())
    return;

  engine->form_highlights_.push_back(rect);
}

// static
void PDFiumFormFiller::Form_SetCursor(FPDF_FORMFILLINFO* param,
                                      int cursor_type) {
  // We don't need this since it's not enough to change the cursor in all
  // scenarios.  Instead, we check which form field we're under in OnMouseMove.
}

// static
int PDFiumFormFiller::Form_SetTimer(FPDF_FORMFILLINFO* param,
                                    int elapse,
                                    TimerCallback timer_func) {
  PDFiumEngine* engine = GetEngine(param);
  base::TimeDelta elapse_time = base::TimeDelta::FromMilliseconds(elapse);
  engine->formfill_timers_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(++engine->next_formfill_timer_id_),
      std::forward_as_tuple(elapse_time, timer_func));
  engine->client_->ScheduleCallback(engine->next_formfill_timer_id_,
                                    elapse_time);
  return engine->next_formfill_timer_id_;
}

// static
void PDFiumFormFiller::Form_KillTimer(FPDF_FORMFILLINFO* param, int timer_id) {
  PDFiumEngine* engine = GetEngine(param);
  engine->formfill_timers_.erase(timer_id);
}

// static
FPDF_SYSTEMTIME PDFiumFormFiller::Form_GetLocalTime(FPDF_FORMFILLINFO* param) {
  base::Time time = base::Time::Now();
  base::Time::Exploded exploded;
  time.LocalExplode(&exploded);

  FPDF_SYSTEMTIME rv;
  rv.wYear = exploded.year;
  rv.wMonth = exploded.month;
  rv.wDayOfWeek = exploded.day_of_week;
  rv.wDay = exploded.day_of_month;
  rv.wHour = exploded.hour;
  rv.wMinute = exploded.minute;
  rv.wSecond = exploded.second;
  rv.wMilliseconds = exploded.millisecond;
  return rv;
}

// static
void PDFiumFormFiller::Form_OnChange(FPDF_FORMFILLINFO* param) {
  PDFiumEngine* engine = GetEngine(param);
  engine->SetEditMode(true);
}

// static
FPDF_PAGE PDFiumFormFiller::Form_GetPage(FPDF_FORMFILLINFO* param,
                                         FPDF_DOCUMENT document,
                                         int page_index) {
  PDFiumEngine* engine = GetEngine(param);
  if (!engine->PageIndexInBounds(page_index))
    return nullptr;
  return engine->pages_[page_index]->GetPage();
}

// static
FPDF_PAGE PDFiumFormFiller::Form_GetCurrentPage(FPDF_FORMFILLINFO* param,
                                                FPDF_DOCUMENT document) {
  PDFiumEngine* engine = GetEngine(param);
  int index = engine->last_page_mouse_down_;
  if (index == -1) {
    index = engine->GetMostVisiblePage();
    if (index == -1) {
      NOTREACHED();
      return nullptr;
    }
  }

  return engine->pages_[index]->GetPage();
}

// static
int PDFiumFormFiller::Form_GetRotation(FPDF_FORMFILLINFO* param,
                                       FPDF_PAGE page) {
  return 0;
}

// static
void PDFiumFormFiller::Form_ExecuteNamedAction(FPDF_FORMFILLINFO* param,
                                               FPDF_BYTESTRING named_action) {
  PDFiumEngine* engine = GetEngine(param);
  std::string action(named_action);
  if (action == "Print") {
    engine->client_->Print();
    return;
  }

  int index = engine->last_page_mouse_down_;
  /* Don't try to calculate the most visible page if we don't have a left click
     before this event (this code originally copied Form_GetCurrentPage which of
     course needs to do that and which doesn't have recursion). This can end up
     causing infinite recursion. See http://crbug.com/240413 for more
     information. Either way, it's not necessary for the spec'd list of named
     actions.
  if (index == -1)
    index = engine->GetMostVisiblePage();
  */
  if (index == -1)
    return;

  // This is the only list of named actions per the spec (see 12.6.4.11). Adobe
  // Reader supports more, like FitWidth, but since they're not part of the spec
  // and we haven't got bugs about them, no need to now.
  if (action == "NextPage") {
    engine->ScrollToPage(index + 1);
  } else if (action == "PrevPage") {
    engine->ScrollToPage(index - 1);
  } else if (action == "FirstPage") {
    engine->ScrollToPage(0);
  } else if (action == "LastPage") {
    engine->ScrollToPage(engine->pages_.size() - 1);
  }
}

// static
void PDFiumFormFiller::Form_SetTextFieldFocus(FPDF_FORMFILLINFO* param,
                                              FPDF_WIDESTRING value,
                                              FPDF_DWORD valueLen,
                                              FPDF_BOOL is_focus) {
  // Do nothing for now.
  // TODO(gene): use this signal to trigger OSK.
}

// static
void PDFiumFormFiller::Form_DoURIAction(FPDF_FORMFILLINFO* param,
                                        FPDF_BYTESTRING uri) {
  PDFiumEngine* engine = GetEngine(param);
  engine->client_->NavigateTo(std::string(uri),
                              WindowOpenDisposition::CURRENT_TAB);
}

// static
void PDFiumFormFiller::Form_DoGoToAction(FPDF_FORMFILLINFO* param,
                                         int page_index,
                                         int zoom_mode,
                                         float* position_array,
                                         int size_of_array) {
  PDFiumEngine* engine = GetEngine(param);
  engine->ScrollToPage(page_index);
}

#if defined(PDF_ENABLE_XFA)

// static
void PDFiumFormFiller::Form_EmailTo(FPDF_FORMFILLINFO* param,
                                    FPDF_FILEHANDLER* file_handler,
                                    FPDF_WIDESTRING to,
                                    FPDF_WIDESTRING subject,
                                    FPDF_WIDESTRING cc,
                                    FPDF_WIDESTRING bcc,
                                    FPDF_WIDESTRING message) {
  std::string to_str = WideStringToString(to);
  std::string subject_str = WideStringToString(subject);
  std::string cc_str = WideStringToString(cc);
  std::string bcc_str = WideStringToString(bcc);
  std::string message_str = WideStringToString(message);

  PDFiumEngine* engine = GetEngine(param);
  engine->client_->Email(to_str, cc_str, bcc_str, subject_str, message_str);
}

// static
void PDFiumFormFiller::Form_DisplayCaret(FPDF_FORMFILLINFO* param,
                                         FPDF_PAGE page,
                                         FPDF_BOOL visible,
                                         double left,
                                         double top,
                                         double right,
                                         double bottom) {}

// static
void PDFiumFormFiller::Form_SetCurrentPage(FPDF_FORMFILLINFO* param,
                                           FPDF_DOCUMENT document,
                                           int page) {
  PDFiumEngine* engine = GetEngine(param);
  pp::Rect page_view_rect = engine->GetPageContentsRect(page);
  engine->ScrolledToYPosition(page_view_rect.height());
  pp::Point pos(1, page_view_rect.height());
  engine->SetScrollPosition(pos);
}

// static
int PDFiumFormFiller::Form_GetCurrentPageIndex(FPDF_FORMFILLINFO* param,
                                               FPDF_DOCUMENT document) {
  PDFiumEngine* engine = GetEngine(param);
  return engine->GetMostVisiblePage();
}

// static
void PDFiumFormFiller::Form_GetPageViewRect(FPDF_FORMFILLINFO* param,
                                            FPDF_PAGE page,
                                            double* left,
                                            double* top,
                                            double* right,
                                            double* bottom) {
  PDFiumEngine* engine = GetEngine(param);
  int page_index = engine->GetVisiblePageIndex(page);
  if (!engine->PageIndexInBounds(page_index)) {
    *left = 0;
    *right = 0;
    *top = 0;
    *bottom = 0;
    return;
  }

  pp::Rect page_view_rect = engine->GetPageContentsRect(page_index);

  float toolbar_height_in_screen_coords =
      engine->GetToolbarHeightInScreenCoords();

  float page_width = FPDF_GetPageWidth(page);
  float page_height = FPDF_GetPageHeight(page);

  // To convert from a screen scale to a page scale, we multiply by
  // (page_height / page_view_rect.height()) and
  // (page_width / page_view_rect.width()),
  // The base point of the page in screen coords is (page_view_rect.x(),
  // page_view_rect.y()).
  // Therefore, to convert an x position from screen to page
  // coords, we use (page_width * (x - base_x) / page_view_rect.width()).
  // For y positions, (page_height * (y - base_y) / page_view_rect.height()).

  // The top-most y position that can be relied to be visible on the screen is
  // the bottom of the toolbar, which is y = toolbar_height_in_screen_coords.
  float screen_top_in_page_coords =
      page_height * (toolbar_height_in_screen_coords - page_view_rect.y()) /
      page_view_rect.height();
  // The bottom-most y position that is visible on the screen is the bottom of
  // the plugin area, which is y = engine->plugin_size_.height().
  float screen_bottom_in_page_coords =
      page_height * (engine->plugin_size_.height() - page_view_rect.y()) /
      page_view_rect.height();
  // The left-most x position that is visible on the screen is the left of the
  // plugin area, which is x = 0.
  float screen_left_in_page_coords =
      page_width * (0 - page_view_rect.x()) / page_view_rect.width();
  // The right-most x position that is visible on the screen is the right of the
  // plugin area, which is x = engine->plugin_size_.width().
  float screen_right_in_page_coords =
      page_width * (engine->plugin_size_.width() - page_view_rect.x()) /
      page_view_rect.width();

  // Return the edge of the screen or of the page, since we're restricted to
  // both.
  *left = std::max(screen_left_in_page_coords, 0.0f);
  *right = std::min(screen_right_in_page_coords, page_width);
  *top = std::max(screen_top_in_page_coords, 0.0f);
  *bottom = std::min(screen_bottom_in_page_coords, page_height);
}

// static
int PDFiumFormFiller::Form_GetPlatform(FPDF_FORMFILLINFO* param,
                                       void* platform,
                                       int length) {
  int platform_flag = -1;

#if defined(WIN32)
  platform_flag = 0;
#elif defined(__linux__)
  platform_flag = 1;
#else
  platform_flag = 2;
#endif

  std::string javascript =
      "alert(\"Platform:" + base::NumberToString(platform_flag) + "\")";

  return platform_flag;
}

// static
FPDF_BOOL PDFiumFormFiller::Form_PopupMenu(FPDF_FORMFILLINFO* param,
                                           FPDF_PAGE page,
                                           FPDF_WIDGET widget,
                                           int menu_flag,
                                           float x,
                                           float y) {
  return false;
}

// static
FPDF_BOOL PDFiumFormFiller::Form_PostRequestURL(FPDF_FORMFILLINFO* param,
                                                FPDF_WIDESTRING url,
                                                FPDF_WIDESTRING data,
                                                FPDF_WIDESTRING content_type,
                                                FPDF_WIDESTRING encode,
                                                FPDF_WIDESTRING header,
                                                FPDF_BSTR* response) {
  std::string url_str = WideStringToString(url);
  std::string data_str = WideStringToString(data);
  std::string content_type_str = WideStringToString(content_type);
  std::string encode_str = WideStringToString(encode);
  std::string header_str = WideStringToString(header);

  std::string javascript = "alert(\"Post:" + url_str + "," + data_str + "," +
                           content_type_str + "," + encode_str + "," +
                           header_str + "\")";
  return true;
}

// static
FPDF_BOOL PDFiumFormFiller::Form_PutRequestURL(FPDF_FORMFILLINFO* param,
                                               FPDF_WIDESTRING url,
                                               FPDF_WIDESTRING data,
                                               FPDF_WIDESTRING encode) {
  std::string url_str = WideStringToString(url);
  std::string data_str = WideStringToString(data);
  std::string encode_str = WideStringToString(encode);

  std::string javascript =
      "alert(\"Put:" + url_str + "," + data_str + "," + encode_str + "\")";

  return true;
}

// static
void PDFiumFormFiller::Form_UploadTo(FPDF_FORMFILLINFO* param,
                                     FPDF_FILEHANDLER* file_handle,
                                     int file_flag,
                                     FPDF_WIDESTRING to) {
  std::string to_str = WideStringToString(to);
  // TODO: needs the full implementation of form uploading
}

// static
FPDF_LPFILEHANDLER PDFiumFormFiller::Form_DownloadFromURL(
    FPDF_FORMFILLINFO* param,
    FPDF_WIDESTRING url) {
  // NOTE: Think hard about the security implications before allowing
  // a PDF file to perform this action.
  return nullptr;
}

// static
FPDF_FILEHANDLER* PDFiumFormFiller::Form_OpenFile(FPDF_FORMFILLINFO* param,
                                                  int file_flag,
                                                  FPDF_WIDESTRING url,
                                                  const char* mode) {
  // NOTE: Think hard about the security implications before allowing
  // a PDF file to perform this action.
  return nullptr;
}

// static
void PDFiumFormFiller::Form_GotoURL(FPDF_FORMFILLINFO* param,
                                    FPDF_DOCUMENT document,
                                    FPDF_WIDESTRING url) {
  std::string url_str = WideStringToString(url);
  // TODO: needs to implement GOTO URL action
}

// static
int PDFiumFormFiller::Form_GetLanguage(FPDF_FORMFILLINFO* param,
                                       void* language,
                                       int length) {
  return 0;
}

#endif  // defined(PDF_ENABLE_XFA)

// static
int PDFiumFormFiller::Form_Alert(IPDF_JSPLATFORM* param,
                                 FPDF_WIDESTRING message,
                                 FPDF_WIDESTRING title,
                                 int type,
                                 int icon) {
  // See fpdfformfill.h for these values.
  enum AlertType {
    ALERT_TYPE_OK = 0,
    ALERT_TYPE_OK_CANCEL,
    ALERT_TYPE_YES_ON,
    ALERT_TYPE_YES_NO_CANCEL
  };

  enum AlertResult {
    ALERT_RESULT_OK = 1,
    ALERT_RESULT_CANCEL,
    ALERT_RESULT_NO,
    ALERT_RESULT_YES
  };

  PDFiumEngine* engine = GetEngine(param);
  std::string message_str = WideStringToString(message);
  if (type == ALERT_TYPE_OK) {
    engine->client_->Alert(message_str);
    return ALERT_RESULT_OK;
  }

  bool rv = engine->client_->Confirm(message_str);
  if (type == ALERT_TYPE_OK_CANCEL)
    return rv ? ALERT_RESULT_OK : ALERT_RESULT_CANCEL;
  return rv ? ALERT_RESULT_YES : ALERT_RESULT_NO;
}

// static
void PDFiumFormFiller::Form_Beep(IPDF_JSPLATFORM* param, int type) {
  // Beeps are annoying, and not possible using javascript, so ignore for now.
}

// static
int PDFiumFormFiller::Form_Response(IPDF_JSPLATFORM* param,
                                    FPDF_WIDESTRING question,
                                    FPDF_WIDESTRING title,
                                    FPDF_WIDESTRING default_response,
                                    FPDF_WIDESTRING label,
                                    FPDF_BOOL password,
                                    void* response,
                                    int length) {
  std::string question_str = WideStringToString(question);
  std::string default_str = WideStringToString(default_response);

  PDFiumEngine* engine = GetEngine(param);
  std::string rv = engine->client_->Prompt(question_str, default_str);
  base::string16 rv_16 = base::UTF8ToUTF16(rv);
  int rv_bytes = rv_16.size() * sizeof(base::char16);
  if (response) {
    int bytes_to_copy = rv_bytes < length ? rv_bytes : length;
    memcpy(response, rv_16.c_str(), bytes_to_copy);
  }
  return rv_bytes;
}

// static
int PDFiumFormFiller::Form_GetFilePath(IPDF_JSPLATFORM* param,
                                       void* file_path,
                                       int length) {
  PDFiumEngine* engine = GetEngine(param);
  std::string rv = engine->client_->GetURL();
  if (file_path && rv.size() <= static_cast<size_t>(length))
    memcpy(file_path, rv.c_str(), rv.size());
  return rv.size();
}

// static
void PDFiumFormFiller::Form_Mail(IPDF_JSPLATFORM* param,
                                 void* mail_data,
                                 int length,
                                 FPDF_BOOL ui,
                                 FPDF_WIDESTRING to,
                                 FPDF_WIDESTRING subject,
                                 FPDF_WIDESTRING cc,
                                 FPDF_WIDESTRING bcc,
                                 FPDF_WIDESTRING message) {
  // Note: |mail_data| and |length| are ignored. We don't handle attachments;
  // there is no way with mailto.
  std::string to_str = WideStringToString(to);
  std::string cc_str = WideStringToString(cc);
  std::string bcc_str = WideStringToString(bcc);
  std::string subject_str = WideStringToString(subject);
  std::string message_str = WideStringToString(message);

  PDFiumEngine* engine = GetEngine(param);
  engine->client_->Email(to_str, cc_str, bcc_str, subject_str, message_str);
}

// static
void PDFiumFormFiller::Form_Print(IPDF_JSPLATFORM* param,
                                  FPDF_BOOL ui,
                                  int start,
                                  int end,
                                  FPDF_BOOL silent,
                                  FPDF_BOOL shrink_to_fit,
                                  FPDF_BOOL print_as_image,
                                  FPDF_BOOL reverse,
                                  FPDF_BOOL annotations) {
  // No way to pass the extra information to the print dialog using JavaScript.
  // Just opening it is fine for now.
  PDFiumEngine* engine = GetEngine(param);
  engine->client_->Print();
}

// static
void PDFiumFormFiller::Form_SubmitForm(IPDF_JSPLATFORM* param,
                                       void* form_data,
                                       int length,
                                       FPDF_WIDESTRING url) {
  std::string url_str = WideStringToString(url);
  PDFiumEngine* engine = GetEngine(param);
  engine->client_->SubmitForm(url_str, form_data, length);
}

// static
void PDFiumFormFiller::Form_GotoPage(IPDF_JSPLATFORM* param, int page_number) {
  PDFiumEngine* engine = GetEngine(param);
  engine->ScrollToPage(page_number);
}

// static
PDFiumEngine* PDFiumFormFiller::GetEngine(FPDF_FORMFILLINFO* info) {
  auto* form_filler = static_cast<PDFiumFormFiller*>(info);
  return form_filler->engine_;
}

// static
PDFiumEngine* PDFiumFormFiller::GetEngine(IPDF_JSPLATFORM* platform) {
  auto* form_filler = static_cast<PDFiumFormFiller*>(platform);
  return form_filler->engine_;
}

}  // namespace chrome_pdf
