// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_CORE_COMMON_SAVE_PASSWORD_PROGRESS_LOGGER_H_
#define COMPONENTS_AUTOFILL_CORE_COMMON_SAVE_PASSWORD_PROGRESS_LOGGER_H_

#include <stddef.h>

#include <string>

#include "base/macros.h"
#include "url/gurl.h"

namespace base {
class Value;
}

namespace autofill {

struct PasswordForm;

// When logging decisions made by password management code about whether to
// offer user-entered credentials for saving or not, do use this class. It
// offers a suite of convenience methods to format and scrub logs. The methods
// have built-in privacy protections (never include a password, scrub URLs), so
// that the result is appropriate for display on the internals page.
//
// To use this class, the method SendLog needs to be overriden to send the logs
// for display as appropriate.
//
// TODO(vabr): Logically, this class belongs to the password_manager component.
// But the PasswordAutofillAgent needs to use it, so until that agent is in a
// third component, shared by autofill and password_manager, this helper needs
// to stay in autofill as well.
class SavePasswordProgressLogger {
 public:
  // IDs of strings allowed in the logs: for security reasons, we only pass the
  // IDs from the renderer, and map them to strings in the browser.
  enum StringID {
    STRING_DECISION_ASK,
    STRING_DECISION_DROP,
    STRING_DECISION_SAVE,
    STRING_OTHER,
    STRING_SCHEME_HTML,
    STRING_SCHEME_BASIC,
    STRING_SCHEME_DIGEST,
    STRING_SCHEME_USERNAME_ONLY,
    STRING_SCHEME_MESSAGE,
    STRING_SIGNON_REALM,
    STRING_ORIGIN,
    STRING_ACTION,
    STRING_USERNAME_ELEMENT,
    STRING_PASSWORD_ELEMENT,
    STRING_NEW_PASSWORD_ELEMENT,
    STRING_PASSWORD_GENERATED,
    STRING_TIMES_USED,
    STRING_PSL_MATCH,
    STRING_NAME_OR_ID,
    STRING_MESSAGE,
    STRING_SET_AUTH_METHOD,
    STRING_AUTHENTICATION_HANDLED,
    STRING_LOGINHANDLER_FORM,
    STRING_SEND_PASSWORD_FORMS_METHOD,
    STRING_SECURITY_ORIGIN,
    STRING_SECURITY_ORIGIN_FAILURE,
    STRING_WEBPAGE_EMPTY,
    STRING_NUMBER_OF_ALL_FORMS,
    STRING_FORM_FOUND_ON_PAGE,
    STRING_FORM_IS_VISIBLE,
    STRING_FORM_IS_PASSWORD,
    STRING_FORM_IS_NOT_PASSWORD,
    STRING_WILL_SUBMIT_FORM_METHOD,
    STRING_HTML_FORM_FOR_SUBMIT,
    STRING_CREATED_PASSWORD_FORM,
    STRING_SUBMITTED_PASSWORD_REPLACED,
    STRING_DID_START_PROVISIONAL_LOAD_METHOD,
    STRING_FRAME_NOT_MAIN_FRAME,
    STRING_PROVISIONALLY_SAVED_FORM_FOR_FRAME,
    STRING_PASSWORD_FORM_FOUND_ON_PAGE,
    STRING_PASSWORD_FORM_NOT_FOUND_ON_PAGE,
    STRING_PROVISIONALLY_SAVE_PASSWORD_METHOD,
    STRING_PROVISIONALLY_SAVE_PASSWORD_FORM,
    STRING_IS_SAVING_ENABLED,
    STRING_EMPTY_PASSWORD,
    STRING_EXACT_MATCH,
    STRING_MATCH_WITHOUT_ACTION,
    STRING_ORIGINS_MATCH,
    STRING_MATCHING_NOT_COMPLETE,
    STRING_FORM_BLACKLISTED,
    STRING_INVALID_FORM,
    STRING_SYNC_CREDENTIAL,
    STRING_BLOCK_PASSWORD_SAME_ORIGIN_INSECURE_SCHEME,
    STRING_PROVISIONALLY_SAVED_FORM,
    STRING_IGNORE_POSSIBLE_USERNAMES,
    STRING_ON_PASSWORD_FORMS_RENDERED_METHOD,
    STRING_ON_SAME_DOCUMENT_NAVIGATION,
    STRING_ON_ASK_USER_OR_SAVE_PASSWORD,
    STRING_CAN_PROVISIONAL_MANAGER_SAVE_METHOD,
    STRING_NO_PROVISIONAL_SAVE_MANAGER,
    STRING_NUMBER_OF_VISIBLE_FORMS,
    STRING_PASSWORD_FORM_REAPPEARED,
    STRING_SAVING_DISABLED,
    STRING_NO_MATCHING_FORM,
    STRING_SSL_ERRORS_PRESENT,
    STRING_ONLY_VISIBLE,
    STRING_SHOW_PASSWORD_PROMPT,
    STRING_PASSWORDMANAGER_AUTOFILL,
    STRING_PASSWORDMANAGER_AUTOFILLHTTPAUTH,
    STRING_PASSWORDMANAGER_SHOW_INITIAL_PASSWORD_ACCOUNT_SUGGESTIONS,
    STRING_WAIT_FOR_USERNAME,
    STRING_LOGINMODELOBSERVER_PRESENT,
    STRING_WAS_LAST_NAVIGATION_HTTP_ERROR_METHOD,
    STRING_HTTP_STATUS_CODE,
    STRING_PROVISIONALLY_SAVED_FORM_IS_NOT_HTML,
    STRING_PROCESS_MATCHES_METHOD,
    STRING_BEST_SCORE,
    STRING_ON_GET_STORE_RESULTS_METHOD,
    STRING_NUMBER_RESULTS,
    STRING_FETCH_METHOD,
    STRING_NO_STORE,
    STRING_CREATE_LOGIN_MANAGERS_METHOD,
    STRING_OLD_NUMBER_LOGIN_MANAGERS,
    STRING_NEW_NUMBER_LOGIN_MANAGERS,
    STRING_PASSWORD_MANAGEMENT_ENABLED_FOR_CURRENT_PAGE,
    STRING_SHOW_LOGIN_PROMPT_METHOD,
    STRING_NEW_UI_STATE,
    STRING_FORM_NOT_AUTOFILLED,
    STRING_CHANGE_PASSWORD_FORM,
    STRING_PROCESS_FRAME_METHOD,
    STRING_FORM_SIGNATURE,
    STRING_ADDING_SIGNATURE,
    STRING_FORM_FETCHER_STATE,
    STRING_UNOWNED_INPUTS_VISIBLE,
    STRING_ON_FILL_PASSWORD_FORM_METHOD,
    STRING_ON_SHOW_INITIAL_PASSWORD_ACCOUNT_SUGGESTIONS,
    STRING_AMBIGUOUS_OR_EMPTY_NAMES,
    STRING_NUMBER_OF_POTENTIAL_FORMS_TO_FILL,
    STRING_CONTAINS_FILLABLE_USERNAME_FIELD,
    STRING_USERNAME_FIELD_NAME_EMPTY,
    STRING_PASSWORD_FIELD_NAME_EMPTY,
    STRING_FORM_DATA_WAIT,
    STRING_FILL_USERNAME_AND_PASSWORD_METHOD,
    STRING_USERNAMES_MATCH,
    STRING_MATCH_IN_ADDITIONAL,
    STRING_USERNAME_FILLED,
    STRING_PASSWORD_FILLED,
    STRING_FORM_NAME,
    STRING_FIELDS,
    STRING_SERVER_PREDICTIONS,
    STRING_FORM_VOTES,
    STRING_REUSE_FOUND,
    STRING_GENERATION_DISABLED_SAVING_DISABLED,
    STRING_GENERATION_DISABLED_NO_SYNC,
    STRING_GENERATION_RENDERER_ENABLED,
    STRING_GENERATION_RENDERER_INVALID_PASSWORD_FORM,
    STRING_GENERATION_RENDERER_POSSIBLE_ACCOUNT_CREATION_FORMS,
    STRING_GENERATION_RENDERER_NO_PASSWORD_MANAGER_ACCESS,
    STRING_GENERATION_RENDERER_FORM_ALREADY_FOUND,
    STRING_GENERATION_RENDERER_NO_POSSIBLE_CREATION_FORMS,
    STRING_GENERATION_RENDERER_NOT_BLACKLISTED,
    STRING_GENERATION_RENDERER_AUTOCOMPLETE_ATTRIBUTE,
    STRING_GENERATION_RENDERER_NO_SERVER_SIGNAL,
    STRING_GENERATION_RENDERER_ELIGIBLE_FORM_FOUND,
    STRING_GENERATION_RENDERER_NO_FIELD_FOUND,
    STRING_GENERATION_RENDERER_SHOW_GENERATION_POPUP,
    STRING_GENERATION_RENDERER_GENERATED_PASSWORD_ACCEPTED,
    STRING_SUCCESSFUL_SUBMISSION_INDICATOR_EVENT,
    STRING_MAIN_FRAME_ORIGIN,
    STRING_IS_FORM_TAG,
    STRING_FORM_PARSING_INPUT,
    STRING_FORM_PARSING_OUTPUT,
    STRING_INVALID,  // Represents a string returned in a case of an error.
    STRING_MAX = STRING_INVALID
  };

  SavePasswordProgressLogger();
  virtual ~SavePasswordProgressLogger();

  // Call these methods to log information. They sanitize the input and call
  // SendLog to pass it for display.
  void LogPasswordForm(StringID label, const PasswordForm& form);
  void LogHTMLForm(StringID label,
                   const std::string& name_or_id,
                   const GURL& action);
  void LogURL(StringID label, const GURL& url);
  void LogBoolean(StringID label, bool truth_value);
  void LogNumber(StringID label, int signed_number);
  void LogNumber(StringID label, size_t unsigned_number);
  void LogMessage(StringID message);

  // Removes privacy sensitive parts of |url| (currently all but host and
  // scheme).
  static std::string ScrubURL(const GURL& url);

 protected:
  // Sends |log| immediately for display.
  virtual void SendLog(const std::string& log) = 0;

  // Converts |log| and its |label| to a string and calls SendLog on the result.
  void LogValue(StringID label, const base::Value& log);

  // Replaces all characters satisfying IsUnwantedInElementID with a ' '.
  // This damages some valid HTML element IDs or names, but it is likely that it
  // will be still possible to match the scrubbed string to the original ID or
  // name in the HTML doc. That's good enough for the logging purposes, and
  // provides some security benefits.
  static std::string ScrubElementID(const base::string16& element_id);

  // The UTF-8 version of the function above.
  static std::string ScrubElementID(std::string element_id);

  // Translates the StringID values into the corresponding strings.
  static std::string GetStringFromID(SavePasswordProgressLogger::StringID id);

 private:
  DISALLOW_COPY_AND_ASSIGN(SavePasswordProgressLogger);
};

}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CORE_COMMON_SAVE_PASSWORD_PROGRESS_LOGGER_H_
